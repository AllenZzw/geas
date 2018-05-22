module S = Stream
module H = Hashtbl
module Dy = DynArray

module U = Util

open Token
module P = Parser

module Pr = Problem
module Simp = Simplify
module Pol = Polarity

module Sol = Solver
module At = Atom

module B = Builtins

open Build

exception Unknown_constraint of string

let put = Format.fprintf

let print_atom problem env =
  (* Build translation table *)
  let ivar_names = H.create 17 in
  let atom_names = H.create 17 in
  Dy.iteri (fun idx info ->
    H.add ivar_names (Sol.ivar_pred env.ivars.(idx)) info.Pr.id
  ) problem.Pr.ivals ;
  Dy.iteri (fun idx (id, _) -> H.add atom_names env.bvars.(idx) id) problem.Pr.bvals ;
  (* Now, the actual function *)
  (fun fmt at ->
    try
      let id = H.find ivar_names at.At.pid in
      Format.fprintf fmt "%s >= %s" id (Int64.to_string @@ At.to_int at.At.value)
    with Not_found -> try
      let id = H.find ivar_names (Int32.logxor at.At.pid Int32.one) in
      Format.fprintf fmt "%s <= %s" id (Int64.to_string @@ At.to_int @@ At.inv at.At.value)
    with Not_found -> try
      let id = H.find atom_names at in
      Format.fprintf fmt "%s" id
    with Not_found -> try
      let id = H.find atom_names (At.neg at) in
      Format.fprintf fmt "not %s" id
    with Not_found -> Format.fprintf fmt "??")

let print_nogood problem env =
  let pp_atom = print_atom problem env in
  (fun fmt nogood ->
    Util.print_array ~pre:"%% @[[" ~post:"]@]@." ~sep:",@ " pp_atom fmt nogood)

(* Replace variable identifiers with the corresponding
 * intvar/atom *)
let rec resolve_expr env expr =
  match expr with
  | Pr.Ilit v -> Pr.Ilit v
  | Pr.Ivar iv -> Pr.Ivar env.ivars.(iv)
  | Pr.Blit b -> Pr.Blit b
  | Pr.Bvar bv -> Pr.Bvar env.bvars.(bv)
  | Pr.Set dom -> Pr.Set dom
  | Pr.Arr es -> Pr.Arr (Array.map (resolve_expr env) es)

(* Evaluate an expression under a model *)
let rec eval_expr env model expr =
  match expr with
  | Pr.Ilit v -> Pr.Ilit v
  | Pr.Ivar iv -> Pr.Ilit (Sol.int_value model env.ivars.(iv))
  | Pr.Blit b -> Pr.Blit b
  | Pr.Bvar bv -> Pr.Blit (Sol.atom_value model env.bvars.(bv))
  | Pr.Set dom -> Pr.Set dom
  | Pr.Arr es -> Pr.Arr (Array.map (eval_expr env model) es)

let expr_array = function
  | Pr.Arr es -> es
  | _ -> failwith "Expected array" 
               
let get_var_branch ann =
  match ann with
  | Pr.Ann_id "input_order" -> Sol.VAR_INORDER
  | Pr.Ann_id "first_fail" -> Sol.VAR_FIRSTFAIL
  | Pr.Ann_id "smallest" -> Sol.VAR_LEAST
  | Pr.Ann_id "largest" -> Sol.VAR_GREATEST
  | _ -> failwith "Unknown var-branch annotation."

let get_val_branch ann =
  match ann with
  | Pr.Ann_id "indomain_min" -> Sol.VAL_MIN
  | Pr.Ann_id "indomain_max" -> Sol.VAL_MAX 
  | Pr.Ann_id "indomain_split" -> Sol.VAL_SPLIT
  | Pr.Ann_id ("indomain"|"default") -> Sol.VAL_MIN
  | _ -> failwith "Unknown val-branch annotation."

let get_ann_array f ann =
  match ann with
  | Pr.Ann_arr xs -> Array.map f xs
  | _ -> failwith "Expected annotation array."

let collect_array_ivars env expr =
  let vars = 
    match expr with
    | Pr.Arr es ->
      List.rev @@ Array.fold_left (fun vs e ->
        match e with
        | Pr.Ivar v -> env.ivars.(v) :: vs
        | _ -> vs) [] es
    | _ -> failwith "Expected array in collect_array_ivars"
    in
    Array.of_list vars

let force_array_ivars env solver expr =
  match expr with
  | Pr.Arr es -> Array.map (fun e ->
    match e with
    | Pr.Ivar v -> env.ivars.(v)
    | Pr.Ilit k -> Solver.new_intvar solver k k
    | _ -> failwith "Expected value of int-kind in force_array_ivars") es
  | _ -> failwith "Expected array argument to force_array_ivars"

let collect_array_bvars env expr =
  let vars = 
    match expr with
    | Pr.Arr es ->
      List.rev @@ Array.fold_left (fun vs e ->
        match e with
        | Pr.Bvar v -> env.bvars.(v) :: vs
        | _ -> vs) [] es
    | _ -> failwith "Expected array in collect_array_ivars"
    in
    Array.of_list vars

let force_array_bvars env expr =
  match expr with
  | Pr.Arr es -> Array.map (fun e ->
    match e with
    | Pr.Bvar v -> env.bvars.(v)
    | Pr.Blit b -> if b then At.at_True else At.neg At.at_True
    | _ -> failwith "Expected bool-sorted term in force_array_bvars"
    ) es
  | _ -> failwith "Expected array argument to force_array_bvars"

let is_search_ann ann =
  match ann with
  | Pr.Ann_call (("seq_search"|"int_search"|"bool_search"|"bool_priority"|"int_priority"), _) -> true
  | _ -> false

let rec parse_branching problem env solver ann =
  match ann with  
  | Pr.Ann_call ("seq_search", args) | Pr.Ann_call ("warm_start_array", args) ->
      let sub = get_ann_array (fun x -> x) args.(0) in
      Sol.seq_brancher (Array.map (parse_branching problem env solver) sub)
  | Pr.Ann_call ("int_search", args) ->
      let varb = get_var_branch args.(1) in
      let valb = get_val_branch args.(2) in
      let vars = collect_array_ivars env (Pr.resolve_ann problem args.(0)) in
      Sol.new_int_brancher varb valb vars
  | Pr.Ann_call ("bool_search", args) ->
      let varb = get_var_branch args.(1) in
      let valb = get_val_branch args.(2) in
      let vars = collect_array_bvars env (Pr.resolve_ann problem args.(0)) in
      Sol.new_bool_brancher varb valb vars
  | Pr.Ann_call ("int_priority", args) ->
    let varb = get_var_branch args.(2) in
    let sel = force_array_ivars env solver (Pr.resolve_ann problem args.(0)) in
    let sub = get_ann_array (parse_branching problem env solver) args.(1) in
    Sol.new_int_priority_brancher varb sel sub
  | Pr.Ann_call ("bool_priority", args) ->
    let varb = get_var_branch args.(2) in
    let sel = force_array_bvars env (Pr.resolve_ann problem args.(0)) in
    let sub = get_ann_array (parse_branching problem env solver) args.(1) in
    Sol.new_bool_priority_brancher varb sel sub
  | Pr.Ann_call ("warm_start", args) ->
    let xs = force_array_ivars env solver (Pr.resolve_ann problem args.(0)) in
    let cs = Pr.get_array Pr.get_int (Pr.resolve_ann problem args.(1)) in
    assert (Array.length xs = Array.length cs) ;
    Sol.warmstart_brancher
      (Array.init (Array.length xs) (fun i -> Sol.ivar_eq xs.(i) cs.(i)))
  | _ -> failwith "Unknown search annotation"

let rec parse_branchings problem env solver anns =
  let rec aux acc anns =
    match anns with
    | [] -> List.rev acc
    | ann :: anns' ->
      if is_search_ann ann  then
        aux (parse_branching problem env solver ann :: acc) anns'
      else
        aux acc anns'
  in
  aux [] anns

(* Returns none if failed *)
let get_array_assumps env in_acc arr =
  let r_assumps = Array.fold_left
    (fun acc elt ->
      match acc, elt with
      | None, _ -> None
      | _, Pr.Blit false -> None
      | Some assumps, Pr.Blit true -> Some assumps
      | Some assumps, Pr.Bvar b -> Some (env.bvars.(b) :: assumps)
      | _ -> failwith "Non-bool in assumption.") (Some in_acc) arr in
  r_assumps

let get_ann_assumps problem env anns =
  let rec aux acc anns =
    match anns with
    | [] -> Some (List.rev acc)
    | ((Pr.Ann_call ("assume", args)) :: anns') -> 
      begin
        match get_array_assumps env acc
                (Pr.get_array (fun x -> x) (Pr.resolve_ann problem args.(0))) with
        | None -> None
        | Some acc' -> aux acc' anns'
      end
    | _ :: anns' -> aux acc anns'
  in aux [] anns
 
let build_branching problem env solver anns =
  let wrap b =
    if !Opts.free then
      Sol.toggle_brancher [|b; Sol.get_brancher solver|]
    else
      b
  in
  match parse_branchings problem env solver anns with
  | [] -> ()
  | [b] ->  Sol.add_brancher solver (wrap b)
  | bs ->
    let b = Sol.seq_brancher (Array.of_list bs) in
     Sol.add_brancher solver (wrap b)

(* Helpers for printing arrays *)
let print_fzn_array p_expr fmt es dims =
(*
  Format.fprintf fmt "array%dd(@[" (Array.length dims) ;
  Util.print_array Pr.print_ann ~sep:",@ " ~pre:"@[" ~post:"@]" fmt dims ;
  Format.fprintf fmt ",@ " ;
  Util.print_array p_expr ~sep:",@ " ~pre:"[@[" ~post:"@]]" fmt es ;
  Format.fprintf fmt "@])"
  *)
  Format.fprintf fmt "array%dd(" (Array.length dims) ;
  Util.print_array Pr.print_ann ~sep:", " ~pre:"" ~post:"" fmt dims ;
  Format.fprintf fmt ", " ;
  Util.print_array p_expr ~sep:", " ~pre:"[" ~post:"]" fmt es ;
  Format.fprintf fmt ")"


(* Print a variable assignment *)
let get_array_dims e_anns =
  match Pr.ann_call_args e_anns "output_array" with
  | Some [| Pr.Ann_arr dims |] -> dims
  | _ -> failwith "Malformed array dimensions"

let print_binding fmt id expr e_anns =
  let rec aux fmt expr =
    match expr with
    | Pr.Ilit v -> Format.pp_print_int fmt v
    | Pr.Blit b -> Format.pp_print_string fmt (if b then "true" else "false")
    | Pr.Arr es -> print_fzn_array aux fmt es (get_array_dims e_anns)
          (* Util.print_array ~sep:"," ~pre:"[@[" ~post:"@]]" aux fmt es *)
    | _ -> failwith "Expected only literals in solution"
  in
  Format.fprintf fmt "%s = " id ;
  aux fmt expr ;
  Format.fprintf fmt ";@."

let is_output problem expr e_anns =
  match expr with
  | Pr.Ivar iv ->
     let info = Dy.get problem.Pr.ivals iv in
     Pr.ann_has_id info.Pr.ann "output_var"
  | Pr.Bvar bv ->
     let (_, ann) = Dy.get problem.Pr.bvals bv in
     Pr.ann_has_id ann "output_var"
  | Pr.Arr _ -> Pr.ann_has_call e_anns "output_array"
  | _ -> false
  
let is_output_id problem id =
  try
    let (_, anns) = Hashtbl.find problem.Pr.symbols id in
    Pr.ann_has_id anns "output_var" || Pr.ann_has_call anns "output_array"    
  with Not_found -> false

let print_solution fmt problem env model =
  if !Opts.check then
    Check.check_exn problem env.ivars env.bvars model
  else () ;
  Hashtbl.iter (fun id (expr, anns) ->
                if is_output_id problem id || is_output problem expr anns then
                  print_binding fmt id (eval_expr env model expr) anns
                else
                  ()) problem.Pr.symbols

let keys tbl = Hashtbl.fold (fun k v ks -> k :: ks) tbl []
let values tbl = Hashtbl.fold (fun k v vs -> v :: vs) tbl []

let output_vars problem env : (Sol.intvar list) * (Atom.t list) =
  let out_ivars = H.create 17 in
  let out_bvars = H.create 17 in
  (* Recursively collect vars in an expression *)
  let rec collect_expr expr =
    match expr with
    | Pr.Ivar iv -> H.replace out_ivars iv env.ivars.(iv)
    | Pr.Bvar bv -> H.replace out_bvars bv env.bvars.(bv)
    | Pr.Arr es -> Array.iter collect_expr es
    | _ -> ()
  in
  (* Collect vars occuring in any output expressions *)
  Hashtbl.iter (fun id (expr, anns) ->
                if is_output problem expr anns then
                  collect_expr expr
                else
                  ()) problem.Pr.symbols ;
  (values out_ivars, values out_bvars)
  
let block_solution problem env =
  let ivars, atoms = output_vars problem env in
  fun solver model ->
  (*
    let iv_atoms =
      List.map (fun x -> Sol.ivar_ne x (Sol.int_value model x)) ivars in 
      *)
    let iv_low =
      List.map (fun x -> Sol.ivar_lt x (Sol.int_value model x)) ivars in
    let iv_high =
      List.map (fun x -> Sol.ivar_gt x (Sol.int_value model x)) ivars in
    let bv_atoms =
      List.map (fun b -> if Sol.atom_value model b then Atom.neg b else b) atoms in
    (* Sol.post_clause solver (Array.of_list (iv_atoms @ bv_atoms)) *)
    Sol.post_clause solver (Array.of_list (iv_low @ (iv_high @ bv_atoms)))
      
let apply_assumps solver assumps =
  let rec aux assumps =
    match assumps with
      | [] -> true
      | at :: assumps' ->  
        if Sol.assume solver at then
          aux assumps'
        else false
  in aux assumps

(*
let print_nogood fmt nogood =
  let print_atom fmt at =
    if (Int32.to_int at.At.pid) mod 2 == 0 then
      Format.fprintf fmt "p%s >= %s"
        (Int32.to_string (Int32.shift_right at.At.pid 1))
        (Int64.to_string (At.to_int at.At.value))
    else
      Format.fprintf fmt "p%s <= %s"
        (Int32.to_string (Int32.shift_right at.At.pid 1))
        (Int64.to_string (At.to_int @@ At.inv at.At.value))
  in
  Util.print_array ~pre:"%% @[[" ~post:"]@]@." print_atom fmt nogood
  *)
    
let solve_satisfy print_model print_nogood solver assumps =
  let fmt = Format.std_formatter in
  if not (apply_assumps solver assumps) then
    begin
      print_nogood fmt (Sol.get_conflict solver) ;
      Format.fprintf fmt "==========@."
    end
  else
    match Sol.solve solver !Opts.limits with
    | Sol.UNKNOWN -> Format.fprintf fmt "UNKNOWN@."
    | Sol.UNSAT ->
      begin
        if List.length assumps > 0 then
          let nogood = Sol.get_conflict solver in
          print_nogood fmt nogood
      end ; 
      Format.fprintf fmt "==========@."
    | Sol.SAT -> print_model fmt (Sol.get_model solver)

let solve_findall print_model print_nogood block_solution solver assumps =
  let fmt = Format.std_formatter in
  let rec aux max_sols =
    match Sol.solve solver !Opts.limits with
    | Sol.UNKNOWN -> ()
    | Sol.UNSAT -> Format.fprintf fmt "==========@."
    | Sol.SAT ->
       begin
         let model = Sol.get_model solver in
         print_model fmt model ;
         if max_sols <> 1 then
           if block_solution solver model then
             aux (max 0 (max_sols-1))
           else
             Format.fprintf fmt "==========@." 
       end
  in
  if not (apply_assumps solver assumps) then
    Format.fprintf fmt "==========@."
  else
    aux !Opts.max_solutions
          
let decrease_ivar obj_val ivar solver model =
  let model_val = Sol.int_value model ivar in
  (* Format.fprintf Format.err_formatter "%% [[OBJ = %d]]@." model_val ;  *)
  obj_val := Some model_val ;
  Sol.post_atom solver (Sol.ivar_lt ivar model_val)
      
let increase_ivar obj_val ivar solver model =
  let model_val = Sol.int_value model ivar in
  (* Format.fprintf Format.err_formatter "%% [[OBJ = %d]%@." model_val ; *)
  obj_val := Some model_val ;
  Sol.post_atom solver (Sol.ivar_gt ivar model_val)

let relative_limits solver limits =
  let s = Sol.get_statistics solver in
  { Sol.max_time =
      if limits.Sol.max_time > 0. then
        max 0.001 (limits.Sol.max_time -. s.Sol.time)
      else 0. ;
    Sol.max_conflicts =
      if limits.Sol.max_conflicts > 0 then
        max 1 (limits.Sol.max_conflicts - s.Sol.conflicts)
      else 0 }

let probe_objective solver model obj =
  (* Compute bounds *)
  match !Opts.obj_probe_limit with
  | None -> model (* Don't probe *)
  | Some probe_lim ->
    (* Set up limits for probe steps. *)
    let limits =
      let l = !Opts.limits in
      if l.Sol.max_conflicts > 0 then
        (fun () ->
          let rlim = relative_limits solver l in
          { rlim with
              Sol.max_conflicts = min probe_lim (rlim.Sol.max_conflicts) })
      else
        (fun () -> { (relative_limits solver l)
                     with Sol.max_conflicts = probe_lim })
    in
    (* Do some probing *)
    let rec aux model lb ub step =
      if lb = ub then
        model
      else begin
        let mid = max lb (ub - step) in
        if not (Sol.assume solver (Sol.ivar_le obj mid)) then
          (Sol.retract solver ; model)
        else
          match Sol.solve solver (limits ()) with
          | Sol.SAT ->
            let m' = Sol.get_model solver in 
            let ub' = Sol.int_value m' obj in
            (Sol.retract solver ; aux m' lb ub' (2*step))
          | Sol.UNSAT ->
            (Sol.retract solver ; aux model (mid+1) ub 1)
          | Sol.UNKNOWN -> (Sol.retract solver; model)
      end
    in
    aux model (Sol.ivar_lb obj) (Sol.int_value model obj) 1
      
let solve_minimize print_model print_nogood solver obj assumps =
  assert (List.length assumps = 0) ;
  let fmt = Format.std_formatter in
  let limits =
    let l = !Opts.limits in
    (fun () -> relative_limits solver l) in
  let rec aux model =
    (if !Opts.max_solutions < 1 then print_model fmt model) ;
    let obj_val = Sol.int_value model obj in
    if not (Sol.post_atom solver (Sol.ivar_lt obj obj_val)) then
      (begin
        if !Opts.max_solutions > 0 then
          print_model fmt model
       end ;
       Format.fprintf fmt "==========@." ;
       model)
    else
      match Sol.solve solver (limits ()) with
      | Sol.UNKNOWN ->
         (begin
            if !Opts.max_solutions > 0 then
              print_model fmt model
            end ;
          Format.fprintf fmt "INCOMPLETE@." ;
          model)
      | Sol.UNSAT ->
         ((* print_model fmt model ; *)
          begin
            if !Opts.max_solutions > 0 then
              print_model fmt model
            end ;
          Format.fprintf fmt "==========@." ;
          model)
      | Sol.SAT ->
        (* )
        aux (Sol.get_model solver)
        ( *)
        let m' = probe_objective solver (Sol.get_model solver) obj in
        aux m'
        (* *)
  in
  match Sol.solve solver !Opts.limits with
  | Sol.UNKNOWN -> (Format.fprintf fmt "UNKNOWN@." ; None)
  | Sol.UNSAT ->
    (* Format.fprintf fmt "UNSAT@." *)
    (Format.fprintf fmt "==========@." ; None)
  | Sol.SAT ->
    (* Some (aux (Sol.get_model solver)) *)
    Some (aux (probe_objective solver (Sol.get_model solver) obj))

type ovar_state = {
  coeff : int ;
  lb : int ;
  residual : int ;
}

let init_thresholds solver obj =
  let thresholds = H.create 17 in
  let min = ref 0 in
  List.iter (fun (c, x) -> 
    let l = Sol.ivar_lb x in
    min := !min + c * l ;
    H.add thresholds x { coeff = c ; lb = l ; residual = c ; }
  ) obj ;
  !min, thresholds

let adjust_ovar_state st k = 
  assert (k <= st.residual) ;
  if k < st.residual then
    { st with residual = st.residual - k }
  else
    { coeff = st.coeff ;
      lb = st.lb+1 ;
      residual = st.coeff; }

let update_thresholds thresholds bounds =
  let delta = Array.fold_left (fun d (x, b) ->
    let st = H.find thresholds x in
    assert (b > st.lb) ;
    min d st.residual) max_int bounds in
  Array.iter (fun (x, _) ->
    let st = H.find thresholds x in
    H.replace thresholds x (adjust_ovar_state st delta)) bounds ;
  delta

let log_thresholds thresholds =
  let fmt = Format.err_formatter in
  Format.fprintf fmt "{|@[<hov 2>" ;
  Hashtbl.iter (fun k st -> Format.fprintf fmt "@ [%d:%d,%d,%d]"
    (Sol.ivar_pred k |> Int32.to_int) st.coeff st.lb st.residual) thresholds ;
  Format.fprintf fmt "@ |}@]@."

let post_thresholds solver thresholds =
  (* H.fold (fun x t r -> r && Sol.assume solver (Sol.ivar_le x t.lb)) thresholds true *)
  H.fold (fun x t r ->
    if r then
      Sol.assume solver (Sol.ivar_le x t.lb)
    else false) thresholds true

let post_thresholds_upto solver thresholds min_coeff =
  H.fold (fun x st r ->
    if r then
      if st.coeff >= min_coeff then
        (Sol.assume solver (Sol.ivar_le x st.lb))
      else
        true
    else
      false) thresholds true

let build_pred_map solver vars =
  let map = H.create 17 in
  List.iter (fun (_, x) ->
    try
      let _ = H.find map (Sol.ivar_pred x) in
      failwith "Pred already exists."
    with Not_found -> 
      H.add map (Sol.ivar_pred x) x) vars ;
  map

let lb_of_atom pred_map at =
  (* Find the var corresponding to the atom. *)
  let pred = at.At.pid in
  let x = H.find pred_map pred in
  let at0 = Sol.ivar_ge x 0 in
  assert (at0.At.pid = pred) ;
  let delta = Int64.sub at.At.value at0.At.value in
  (x, Int64.to_int delta)
  
let check_core solver core =
  let res =
    if apply_assumps solver (Array.map At.neg core |> Array.to_list) then
      Sol.solve solver (Sol.unlimited ())
    else
      Sol.UNSAT
  in
  assert (res = Sol.UNSAT) ;
  Sol.retract_all solver
  
let post_bool_sum_geq_ solver r bs k =
  let xs = Array.map (fun b ->
    let x = Sol.new_intvar solver 0 1 in
    let at = Sol.ivar_ge x 1 in
    let _ = Sol.post_clause solver [|b ; At.neg at|] in
    let _ = Sol.post_clause solver [|At.neg b ; at|] in
    1, x) bs in
  B.linear_le solver At.at_True (Array.append [|-1, r|] xs) (-k)

let post_bool_sum_geq solver r bs k : bool =
  B.bool_linear_ge solver r (Array.map (fun b -> 1, b) bs) k
  
let process_core solver pred_map thresholds core =
  (* check_core solver core ; *)
  if Array.length core = 1 then
    let _ = Format.fprintf Format.err_formatter "%% singleton@." in
    let (x, b) = lb_of_atom pred_map core.(0) in
    let st = H.find thresholds x in
    assert(b > st.lb) ;
    let cost = st.residual + st.coeff * (b - st.lb - 1) in
    let _ = H.replace thresholds x { coeff = st.coeff ; lb = b ; residual = st.coeff } in
    let okay = Sol.post_atom solver core.(0) in
    assert okay ;
    cost
  else
    begin
      (* Create penalty var *)
      let p = Sol.new_intvar solver 0 (Array.length core - 1) in
      (* Relate penalty to core *)
      let _ = post_bool_sum_geq solver p core (-1) in
      let _ = Sol.post_clause solver core in
      (* Now update thresholds *)
      let bounds = Array.map (lb_of_atom pred_map) core in    
      (*
      let _ = Util.print_array ~pre:"%% @[[" ~post:"]@]@." ~sep:",@ " 
        (fun fmt (x, b) -> Format.fprintf fmt "x%d >= %d" (Sol.ivar_pred x |> Int32.to_int) b) Format.err_formatter bounds in
        *)
      let delta = update_thresholds thresholds bounds in
      H.add pred_map (Sol.ivar_pred p) p ;
      H.add thresholds p { coeff = delta ; lb = 0 ; residual = delta ; } ;
      (* log_thresholds thresholds ; *)
      delta
    end

let log_core_iter =
  let iters = ref 0 in
  (fun lb ->
    incr iters ;
    Format.fprintf Format.err_formatter "%% Unsat core iteration: %d (lb %d).@." !iters lb)

let try_thresholds solver thresholds =
  if post_thresholds solver thresholds then
    let limits = relative_limits solver !Opts.limits in
    Sol.solve solver limits
  else
    Sol.UNSAT

let try_thresholds_upto solver thresholds min_coeff =
  if post_thresholds_upto solver thresholds min_coeff then
    let limits = relative_limits solver !Opts.limits in
    Sol.solve solver limits
  else
    Sol.UNSAT

let rec solve_core' print_nogood solver pred_map thresholds lb =
  log_core_iter lb ;
  (* let okay = post_thresholds solver thresholds in
  let limits = relative_limits solver !Opts.limits in
  match Sol.solve solver limits with *)
  match try_thresholds solver thresholds with
  | Sol.SAT ->
    begin
      let m = Sol.get_model solver in
      Sol.retract_all solver ;
      (* H.iter (fun x st -> assert (Sol.int_value m x <= st.lb)) thresholds ; *)
      (* Check the objective *)
      Some m
    end
  | Sol.UNSAT -> 
    let core = Sol.get_conflict solver in
    begin
      (* print_nogood Format.err_formatter core ;  *)
      Sol.retract_all solver ;
      assert (Array.length core > 0) ;
      let delta = process_core solver pred_map thresholds core in
      solve_core' print_nogood solver pred_map thresholds (lb + delta)
    end
  | Sol.UNKNOWN ->
    begin
      Sol.retract_all solver ;
      None
    end

(* Stratified version of unsat-core based optimization *)
let next_coeff thresholds coeff =
  H.fold (fun x st c ->
    if st.coeff >= coeff then
      c
    else
      max c st.coeff) thresholds 0

type core_result = 
  | Sat of Sol.model
  | Opt of Sol.model
  | Unsat
  | Unknown

let rec solve_core_strat print_model print_nogood solver obj incumbent pred_map thresholds min_coeff lb =
  log_core_iter lb ;
  Format.fprintf Format.err_formatter "%% Min coeff: %d, incumbent value %d@." min_coeff (Solver.int_value incumbent obj) ;
  (* We know there exists a minimum-weight model for [obj.(0), ..., obj.(idx-1)].
   * Now expand to the rest of the variables. *)
  match try_thresholds_upto solver thresholds min_coeff with
  | Sol.SAT ->
    begin
      (* Format.fprintf Format.err_formatter "%% [SAT].@." ; *)
      let m = Sol.get_model solver in
      let _ = print_model Format.std_formatter m in
      (*
      let fmt = Format.err_formatter in
      Format.fprintf fmt "{|@[<hov 2>" ;
      Hashtbl.iter (fun x st -> Format.fprintf fmt "@ [%d:%d|%d]"
        (Sol.ivar_pred x |> Int32.to_int) st.lb (Sol.int_value m x)) thresholds ;
      Format.fprintf fmt "@ |}@]@." ;
      *)
      Sol.retract_all solver ;
      let coeff' = next_coeff thresholds min_coeff in
      if coeff' = 0 then
        Opt m
      else
        (*
        let obj_val = Sol.int_value m obj in
        let _ = Sol.post_atom solver (Sol.ivar_lt obj obj_val) in
        *)
        let m' = if (Sol.int_value m obj) < (Sol.int_value incumbent obj) then m else incumbent in
        solve_core_strat print_model print_nogood solver obj m' pred_map thresholds coeff' lb
    end
  | Sol.UNSAT -> 
    let core = Sol.get_conflict solver in
    begin
      (* print_nogood Format.err_formatter core ;  *)
      Sol.retract_all solver ;
      assert (Array.length core > 0) ;
      if Array.length core = 0 then
        Opt incumbent
      else
        let delta = process_core solver pred_map thresholds core in
        solve_core_strat print_model print_nogood solver obj incumbent pred_map thresholds min_coeff (lb + delta)
    end
  | Sol.UNKNOWN ->
    begin
      Sol.retract_all solver ;
      Sat incumbent
    end

let solve_core print_model print_nogood solver obj_var obj k =
  (* Post penalty thresholds *)
  let limits () = relative_limits solver !Opts.limits in
  match Sol.solve solver (limits ()) with
  | Sol.SAT ->
    (* Found a first model. *)
    let m = Sol.get_model solver in
    begin
      (* Thresholds records how much of each 
       * variable is 'free'. *)
      let pred_map = build_pred_map solver obj in
      let obj_lb, thresholds = init_thresholds solver obj in
      (* match solve_core' print_nogood solver pred_map thresholds (k + obj_lb) with 
      | Some m' -> Opt m'
      | None -> Sat m
      *)
      (*
      match solve_core_strat print_model print_nogood solver obj_var m pred_map thresholds (next_coeff thresholds max_int) (k + obj_lb) with
      | Some m' -> Opt m'
      | None -> Sat m
      *)
      (*
      let obj_val = Sol.int_value m obj_var in
      let _ = Sol.post_atom solver (Sol.ivar_lt obj_var obj_val) in
      *)
      solve_core_strat print_model print_nogood solver obj_var m pred_map thresholds (next_coeff thresholds max_int) (k + obj_lb)
    end
  | Sol.UNSAT -> Unsat
  | Sol.UNKNOWN -> Unknown
 
let print_stats fmt stats obj_val =
  match !Opts.print_stats with
  | Opts.Suppress -> ()
  | Opts.Compact ->  
    begin
      Format.fprintf fmt "%d,%d,%d,%d,%d,%.02f@."
        (match obj_val with
          | Some k -> k
          | None -> 0)
        stats.Sol.solutions
        stats.Sol.restarts
        stats.Sol.conflicts
        stats.Sol.num_learnts
        stats.Sol.time
    end
  | Opts.Verbose ->
    begin
      let _ = match obj_val with
      | Some k -> Format.fprintf fmt "objective %d@." k
      | None -> ()
      in
      Format.fprintf fmt "%d solutions, %d conflicts, %d restarts@." stats.Sol.solutions stats.Sol.conflicts stats.Sol.restarts ;
      Format.fprintf fmt "%.02f seconds.@." stats.Sol.time ;
      Format.fprintf fmt "%d learnts, average size %f@."
        stats.Sol.num_learnts
        ((float_of_int stats.Sol.num_learnt_lits) /. (float_of_int stats.Sol.num_learnts))
    end

let get_options () =
  let defaults = Sol.default_options () in
  let rlimit = !Opts.restart_limit in
  { defaults with
    Sol.restart_limit =
      match rlimit with
      | Some r -> r
      | None -> defaults.Sol.restart_limit }
  
let set_polarity solver env pol_info =
  Array.iteri (fun i ctx ->
    match ctx with
    | { Pol.pos = false ; Pol.neg = true } ->
      Sol.set_bool_polarity solver env.bvars.(i) false
    | { Pol.pos = true ; Pol.neg = false } ->
      Sol.set_bool_polarity solver env.bvars.(i) true
    | _ -> ()
  ) pol_info.Pol.bvars ;
  Array.iteri (fun i ctx -> 
    match ctx with
    | { Pol.pos = false ; Pol.neg = true } ->
      Sol.set_int_polarity env.ivars.(i) false
    | { Pol.pos = true ; Pol.neg = false } ->
      Sol.set_int_polarity env.ivars.(i) true
    | _ -> ()
  ) pol_info.Pol.ivars

let minimize_uc print_model print_nogood solver obj xs k =
    let fmt = Format.std_formatter in
    (* Format.fprintf fmt "[ k = %d ]@." k ; *)
    match solve_core print_model print_nogood solver obj (Array.to_list xs) k with
    | Sat m ->
      (print_model fmt m ;
       Format.fprintf fmt "----------@." ;
       Some m)
    | Opt m ->
      (print_model fmt m ;
      (*
       (* Check optimum *)
         let opt = Sol.int_value m obj in
         begin
           if Sol.assume solver (Sol.ivar_lt obj opt) then
             let res = Sol.solve solver (Solver.unlimited ()) in
             assert (res = Sol.UNSAT)
           else ()
         end ;
         Sol.retract_all solver ;
         *)
       Format.fprintf fmt "==========@." ;
       Some m)
    | Unsat ->
       (Format.fprintf fmt "==========@." ; None)
    | Unknown -> None

let minimize_linear print_model print_nogood solver obj ts k =
  if !Opts.core_opt then
    (* Solve using unsat cores. *)
    let xs = Array.map (fun (c, x) ->
      if c > 0 then
        c, x
      else
        -c, Sol.intvar_neg x) ts in
    minimize_uc print_model print_nogood solver obj xs k
  else
    solve_minimize print_model print_nogood solver obj []

let collect_linterms defs env obj =
  let k = ref 0 in
  let ts = H.create 17 in
  (* Collect the set of terms. *)
  let rec aux coeff obj =
    match defs.(obj) with
    | Simp.Iv_const c -> k := !k + coeff * c
    | Simp.Iv_eq x -> aux coeff x
    | Simp.Iv_opp x -> aux (-coeff) x
    | Simp.Iv_lin (ts, c) ->
      begin
        k := !k + coeff * c ;
        Array.iter (fun (c, x) -> aux (c * coeff) x) ts
      end
    | _ ->
      begin
        let x = env.ivars.(obj) in
        try
          let cx = H.find ts x in
          H.replace ts x (cx + coeff)
        with Not_found -> H.add ts x coeff 
      end
  in
  aux 1 obj ; 
  (* Now produce an array *) 
  let ts_list = Hashtbl.fold (fun x c ts -> (c, x) :: ts) ts [] in
  Array.of_list ts_list, !k

(* Follow the objective chain definition, to set variable polarities. *)
let set_obj_polarity solver idefs bdefs env sign v =
  let seeni = H.create 17 in
  let seenb = H.create 17 in
  let rec auxi sign v =  
    try H.find seeni v
    with Not_found ->
      H.add seeni v () ;
      Sol.set_int_polarity env.ivars.(v) sign ;
      match idefs.(v) with
     (* Aliasing *)
     | Simp.Iv_eq v' -> auxi sign v'
     | Simp.Iv_opp v' -> auxi (not sign) v'
     (* Arithmetic functions *)
     | Simp.Iv_lin (ts, _) ->
      Array.iter (fun (c, x) -> if c > 0 then auxi sign x else auxi (not sign) x) ts
     | Simp.Iv_max xs -> Array.iter (auxi sign) xs
     | Simp.Iv_min xs -> Array.iter (auxi sign) xs
     | Simp.Iv_b2i b -> auxb sign b
     | _ -> ()
   and auxb sign b =
    try H.find seenb b
    with Not_found ->
      H.add seenb b () ;
      Sol.set_bool_polarity solver env.bvars.(b) sign ;
      match bdefs.(v) with
      | Simp.Bv_eq b' -> auxb sign b'
      | Simp.Bv_neg b' -> auxb (not sign) b'
      | Simp.At (x, Simp.Ile, _) -> auxi (not sign) x
      | Simp.At (x, Simp.Igt, _) -> auxi sign x
      | Simp.Bv_or bs -> Array.iter (auxb sign) bs
      | Simp.Bv_and bs -> Array.iter (auxb sign) bs
      | _ -> ()
  in auxi sign v
    
let main () =
  (* Parse the command-line arguments *)
  Arg.parse
    Opts.speclist
      (begin fun infile -> Opts.infile := Some infile end)
      "fzn_geas <options> <inputfile>"
  ;
  Half_reify.initialize () ;
  Registry.initialize () ;
  (* Parse the program *)
  let input = match !Opts.infile with
      | None -> stdin
      | Some file -> open_in file
  in
  let lexer = P.lexer input in
  let orig_problem = P.read_problem lexer in
  (* let pol_ctxs = Polarity.polarity orig_problem in *)
  let s_problem = Simplify.simplify orig_problem in
  let ctxs = Polarity.polarity s_problem in
  let opts = get_options () in
  let solver = Sol.new_solver opts in
  (* Construct the problem *)
  (*
  let problem =
    if !Opts.half_reify then
      Half_reify.half_reify ~ctxs:pol_ctxs problem 
    else problem in
  *)
  let (idefs, bdefs, problem) = s_problem in
  (* Simp.log_reprs idefs bdefs ; *)
  (* let env = build_problem solver problem ctxs idefs bdefs in *)
  let env = Build.build_problem solver s_problem ctxs in
  (* Perform polarity analysis, to set branching *)
  let _ = if !Opts.pol then
    set_polarity solver env ctxs in
  let assumps =
    match get_ann_assumps problem env (snd problem.Pr.objective) with
    | None -> [ At.neg At.at_True ]
    | Some atoms -> atoms
  in
  build_branching problem env solver (snd problem.Pr.objective) ;
  (*
  let problem_HR =
    if !Opts.noop then
      problem
    else
      Half_reify.half_reify problem in
   *)
  let print_model =
    (fun fmt model ->
      if not !Opts.quiet then
        begin
          print_solution fmt problem env model ;
          Format.fprintf fmt "----------@."
        end) in
  let print_nogood = print_nogood problem env in
  let obj_val = ref None in
  begin
  match fst problem.Pr.objective with
  | Pr.Satisfy ->
     if !Opts.max_solutions = 1 then
        solve_satisfy print_model print_nogood solver assumps
     else
       let block = block_solution problem env in
       solve_findall print_model print_nogood block solver assumps
  | Pr.Minimize obj ->
      set_obj_polarity solver idefs bdefs env false obj ;
      let r : Sol.model option = match idefs.(obj) with
        | Simp.Iv_lin _ ->
          let xs, k = collect_linterms idefs env obj in
          (* let xs = Array.map (fun (c, x) -> c, env.ivars.(x)) ts in *)
          minimize_linear print_model print_nogood solver env.ivars.(obj) xs k
        | _ ->
          solve_minimize print_model print_nogood solver env.ivars.(obj) []
      in
      begin
      match r with
      | Some m -> obj_val := Some (Sol.int_value m env.ivars.(obj))
      | None -> ()
      end
  | Pr.Maximize obj ->
      set_obj_polarity solver idefs bdefs env true obj ;
      let r = match idefs.(obj) with
        | Simp.Iv_lin _ ->
          (* let xs = Array.map (fun (c, x) -> -c, env.ivars.(x)) ts in *)
          let xs, k = collect_linterms idefs env obj in
          minimize_linear print_model print_nogood solver (Sol.intvar_neg env.ivars.(obj))
            (Array.map (fun (c, x) -> (-c, x)) xs) (-k)
        | _ ->
          solve_minimize print_model print_nogood solver (Sol.intvar_neg env.ivars.(obj)) []
      in
      begin
      match r with
      | Some m -> obj_val := Some (Sol.int_value m env.ivars.(obj))
      | None -> ()
      end
  end ;
  (* let fmt = Format.std_formatter in *)
  let fmt = Format.err_formatter in
  print_stats fmt (Sol.get_statistics solver) !obj_val

let _ = main ()
