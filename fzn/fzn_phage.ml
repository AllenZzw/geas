module S = Stream
module H = Hashtbl
module Dy = DynArray

module U = Util

open Token
module P = Parser

module Pr = Problem

module Sol = Solver

exception Unknown_constraint of string

let put = Format.fprintf

let make_intvar solver dom =
  let lb, ub = Dom.bounds dom in
  (* Create the var *)
  let v = Sol.new_intvar solver lb ub in
  (* Punch any holes. post_clause should never fail here. *)
  (* FIXME: export bindings for sparse vars. *)
  List.iter
    (fun (hl, hu) ->
     ignore @@
       Sol.post_clause solver
                       [|Sol.ivar_lt v hl ; Sol.ivar_gt v hu|]) (Dom.holes dom)  ;
  (* Then return the constructed variable *)
  v

(* Constraint registry. FIXME: Maybe move to a separate module. *)
(* let registry = H.create 17 *)

type env = { ivars : Sol.intvar array ; bvars : Atom.atom array }

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
               
let post_constraint solver env ident exprs anns = 
  try
    (* let handler = H.find registry ident in *)
    let handler = Registry.lookup ident in
    let args = Array.map (resolve_expr env) exprs in
    handler solver args anns
  with Not_found ->
    (* raise (Unknown_constraint ident) *)
    (Format.fprintf Format.err_formatter "Warning: Ignoring unknown constraint '%s'.@." ident ;
     Registry.register ident (fun _ _ _ -> true) ;
        true)

let build_problem solver problem =
  (* Allocate variables *)
  let ivars =
    Array.init
      (Dy.length problem.Pr.ivals)
      (fun i ->
       let vinfo = Dy.get problem.Pr.ivals i in
       make_intvar solver vinfo.Pr.dom)
  in
  let bvars =
    Array.init
      (Dy.length problem.Pr.bvals)
      (fun i -> Sol.new_boolvar solver)
  in
  let env = { ivars = ivars; bvars = bvars } in
  (* Process constraints *)
  Dy.iteri (fun id ((ident, expr), anns) ->
           Sol.set_cons_id solver (id+1) ;
           ignore @@ post_constraint solver env ident expr anns)
          problem.Pr.constraints ;
  (* Then, return the bindings *)
  env

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
      Array.fold_left (fun vs e ->
        match e with
        | Pr.Ivar v -> env.ivars.(v) :: vs
        | _ -> vs) [] es
    | _ -> failwith "Expected array in collect_array_ivars"
    in
    Array.of_list vars

let rec parse_branching problem env ann =
  match ann with  
  | Pr.Ann_call ("seq_search", args) ->
      let sub = get_ann_array (fun x -> x) args.(0) in
      Sol.seq_brancher (Array.map (parse_branching problem env) sub)
  | Pr.Ann_call ("int_search", args) ->
      let varb = get_var_branch args.(1) in
      let valb = get_val_branch args.(2) in
      let vars = collect_array_ivars env (Pr.resolve_ann problem args.(0)) in
      Sol.new_brancher varb valb vars
  | _ -> failwith "Unknown search annotation"

let build_branching problem env solver anns =
  List.iter (fun ann ->
    Sol.add_brancher solver (parse_branching problem env ann)) anns

(* Print a variable assignment *)
let print_binding fmt id expr =
  let rec aux fmt expr =
    match expr with
    | Pr.Ilit v -> Format.pp_print_int fmt v
    | Pr.Blit b -> Format.pp_print_string fmt (if b then "true" else "false")
    | Pr.Arr es -> Util.print_array aux fmt es
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
  
let print_solution fmt problem env model =
  if !Opts.check then
    Check.check_exn problem env.ivars env.bvars model
  else () ;
  Hashtbl.iter (fun id (expr, anns) ->
                if is_output problem expr anns then
                  print_binding fmt id (eval_expr env model expr)
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
      
let solve_satisfy print_model solver =
  let fmt = Format.std_formatter in
  match Sol.solve solver (-1) with
  | Sol.UNKNOWN -> Format.fprintf fmt "UNKNOWN@."
  | Sol.UNSAT -> Format.fprintf fmt "============@."
  | Sol.SAT -> print_model fmt (Sol.get_model solver)

let solve_findall print_model block_solution solver =
  let fmt = Format.std_formatter in
  let rec aux max_sols =
    match Sol.solve solver (-1) with
    | Sol.UNKNOWN -> ()
    | Sol.UNSAT -> Format.fprintf fmt "============@."
    | Sol.SAT ->
       begin
         let model = Sol.get_model solver in
         print_model fmt model ;
         if max_sols <> 1 then
           if block_solution solver model then
             aux (max 0 (max_sols-1))
           else
             Format.fprintf fmt "===========@." 
       end
  in
  aux !Opts.max_solutions
          
let decrease_ivar ivar solver model =
  let model_val = Sol.int_value model ivar in
  Sol.post_atom solver (Sol.ivar_lt ivar model_val)
      
let increase_ivar ivar solver model =
  let model_val = Sol.int_value model ivar in
  Sol.post_atom solver (Sol.ivar_gt ivar model_val)

let solve_optimize print_model constrain solver =
  let fmt = Format.std_formatter in
  let rec aux model =
    print_model fmt model ;
    if not (constrain solver model) then
      ((* print_model fmt model ; *)
       Format.fprintf fmt "============@.")
    else
      match Sol.solve solver (-1) with
      | Sol.UNKNOWN ->
         ((* print_model fmt model ; *)
          Format.fprintf fmt "INCOMPLETE@.")
      | Sol.UNSAT ->
         ((* print_model fmt model ; *)
          Format.fprintf fmt "==============@.")
      | Sol.SAT -> aux (Sol.get_model solver)
  in
  match Sol.solve solver (-1) with
  | Sol.UNKNOWN -> Format.fprintf fmt "UNKNOWN@."
  | Sol.UNSAT -> Format.fprintf fmt "UNSAT@."
  | Sol.SAT -> aux (Sol.get_model solver)
 
let print_stats fmt stats =
  if !Opts.print_stats then
    Format.fprintf fmt "%d solutions, %d conflicts, %d restarts@." stats.Sol.solutions stats.Sol.conflicts stats.Sol.restarts

let main () =
  (* Parse the command-line arguments *)
  Arg.parse
    Opts.speclist
      (begin fun infile -> Opts.infile := Some infile end)
      "fzn_phage <options> <inputfile>"
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
  let problem = Simplify.simplify orig_problem in
  let solver = Sol.new_solver () in
  (* Do stuff *)
  let env = build_problem solver problem in
  build_branching problem env solver (snd problem.Pr.objective) ;
  (*
  let problem_HR =
    if !Opts.noop then
      problem
    else
      Half_reify.half_reify problem in
   *)
  (*
  if not !Opts.quiet then
    begin
      let output = match !Opts.outfile with
          | None -> stdout
          | Some file -> open_out file in 
      let fmt = Format.formatter_of_out_channel output in
      (* Pr.print fmt problem_HR ; *)
      Pr.print fmt problem ;
      close_out output
    end ;
   *)
  let print_model =
    (fun fmt model ->
      if not !Opts.quiet then
        begin
          print_solution fmt problem env model ;
          Format.fprintf fmt "------------@."
        end) in
  begin
  match fst problem.Pr.objective with
  | Pr.Satisfy ->
     if !Opts.max_solutions = 1 then
        solve_satisfy print_model solver
     else
       let block = block_solution problem env in
       solve_findall print_model block solver
  | Pr.Minimize obj ->
     solve_optimize print_model (decrease_ivar env.ivars.(obj)) solver
  | Pr.Maximize obj ->
     solve_optimize print_model (increase_ivar env.ivars.(obj)) solver
  end ;
  print_stats Format.std_formatter (Sol.get_statistics solver)

let _ = main ()
