#include "dplyr.h"

SEXP new_environment(int size, SEXP parent)  {
  SEXP call = PROTECT(Rf_lang4(Rf_install("new.env"), Rf_ScalarLogical(TRUE), parent, Rf_ScalarInteger(size)));
  SEXP res = Rf_eval(call, R_BaseEnv);
  UNPROTECT(1);
  return res;
}

void dplyr_lazy_vec_chop_grouped(SEXP e, SEXP data, bool rowwise) {
  SEXP names = PROTECT(Rf_getAttrib(data, R_NamesSymbol));
  SEXP groups_df = PROTECT(Rf_getAttrib(data, dplyr::symbols::groups));
  SEXP indices = VECTOR_ELT(groups_df, XLENGTH(groups_df) - 1);
  R_xlen_t n = XLENGTH(data);

  for (R_xlen_t i = 0; i < n; i++) {
    SEXP prom = PROTECT(Rf_allocSExp(PROMSXP));
    SET_PRENV(prom, R_EmptyEnv);
    SEXP column = VECTOR_ELT(data, i);
    if (rowwise && vctrs::vec_is_list(column)) {
      SET_PRCODE(prom, column);
    } else {
      SET_PRCODE(prom, Rf_lang3(dplyr::functions::vec_chop, column, indices));
    }
    SET_PRVALUE(prom, R_UnboundValue);

    Rf_defineVar(Rf_installChar(STRING_ELT(names, i)), prom, e);
    UNPROTECT(1);
  }

  UNPROTECT(2);
}

void dplyr_lazy_vec_chop_ungrouped(SEXP e, SEXP data) {
  SEXP names = PROTECT(Rf_getAttrib(data, R_NamesSymbol));
  R_xlen_t n = XLENGTH(data);

  for (R_xlen_t i = 0; i < n; i++) {
    SEXP prom = PROTECT(Rf_allocSExp(PROMSXP));
    SET_PRENV(prom, R_EmptyEnv);
    SET_PRCODE(prom, Rf_lang2(dplyr::functions::list, VECTOR_ELT(data, i)));
    SET_PRVALUE(prom, R_UnboundValue);

    Rf_defineVar(Rf_installChar(STRING_ELT(names, i)), prom, e);
    UNPROTECT(1);
  }

  UNPROTECT(1);
}

SEXP dplyr_lazy_vec_chop(SEXP data, SEXP caller_env) {
  SEXP e = PROTECT(new_environment(XLENGTH(data), caller_env));
  if (Rf_inherits(data, "grouped_df")) {
    dplyr_lazy_vec_chop_grouped(e, data, false);
  } else if (Rf_inherits(data, "rowwise_df")) {
    dplyr_lazy_vec_chop_grouped(e, data, true);
  } else {
    dplyr_lazy_vec_chop_ungrouped(e, data);
  }
  UNPROTECT(1);
  return e;
}

SEXP dplyr_data_masks_setup(SEXP chops, SEXP data) {
  SEXP names = PROTECT(Rf_getAttrib(data, R_NamesSymbol));

  R_xlen_t n_groups = 1;
  if (Rf_inherits(data, "grouped_df")) {
    SEXP groups_df = PROTECT(Rf_getAttrib(data, dplyr::symbols::groups));
    SEXP indices = VECTOR_ELT(groups_df, XLENGTH(groups_df) - 1);

    n_groups = XLENGTH(indices);
    UNPROTECT(1);
  } else if (Rf_inherits(data, "rowwise_df")) {
    n_groups = vctrs::short_vec_size(data);
  }
  R_xlen_t n_columns = XLENGTH(names);

  // create masks
  R_xlen_t mask_size = XLENGTH(data) + 20;
  SEXP masks = PROTECT(Rf_allocVector(VECSXP, n_groups));
  for (R_xlen_t i = 0; i < n_groups; i++) {
    SET_VECTOR_ELT(masks, i, new_environment(mask_size, R_EmptyEnv));
  }

  for (R_xlen_t i = 0; i < n_columns; i++) {
    SEXP name = Rf_installChar(STRING_ELT(names, i));

    for (R_xlen_t j = 0; j < n_groups; j++) {
      SEXP prom = PROTECT(Rf_allocSExp(PROMSXP));
      SET_PRENV(prom, chops);
      SET_PRCODE(prom, Rf_lang3(dplyr::functions::dot_subset2, name, Rf_ScalarInteger(j + 1)));
      SET_PRVALUE(prom, R_UnboundValue);

      Rf_defineVar(name, prom, VECTOR_ELT(masks, j));
      UNPROTECT(1);
    }
  }

  UNPROTECT(2);
  return masks;
}

SEXP env_resolved(SEXP env, SEXP names) {
  R_xlen_t n = XLENGTH(names);
  SEXP res = PROTECT(Rf_allocVector(LGLSXP, n));

  int* p_res = LOGICAL(res);
  for(R_xlen_t i = 0; i < n; i++) {
    SEXP prom = Rf_findVarInFrame(env, Rf_installChar(STRING_ELT(names, i)));
    p_res[i] = PRVALUE(prom) != R_UnboundValue;
  }

  Rf_namesgets(res, names);
  UNPROTECT(1);
  return res;
}

namespace funs {

SEXP eval_hybrid(SEXP quo, SEXP chops) {
  SEXP call = PROTECT(Rf_lang3(dplyr::functions::eval_hybrid, quo, chops));
  SEXP res = PROTECT(Rf_eval(call, R_BaseEnv));
  UNPROTECT(2);

  return res;
}

}

SEXP dplyr_eval_tidy_all(SEXP quosures, SEXP chops, SEXP masks, SEXP caller_env, SEXP auto_names, SEXP context) {
  R_xlen_t n_expr = XLENGTH(quosures);
  SEXP names = PROTECT(Rf_getAttrib(quosures, R_NamesSymbol));
  R_xlen_t n_masks = XLENGTH(masks);

  // initialize all results
  SEXP res = PROTECT(Rf_allocVector(VECSXP, n_masks));
  for (R_xlen_t i = 0; i < n_masks; i++) {
    SEXP res_i = PROTECT(Rf_allocVector(VECSXP, n_expr));
    Rf_namesgets(res_i, names);
    SET_VECTOR_ELT(res, i, res_i);
    UNPROTECT(1);
  }

  SEXP index_expression = Rf_findVarInFrame(context, Rf_install("index_expression"));
  int *p_index_expression = INTEGER(index_expression);

  SEXP index_group = Rf_findVarInFrame(context, Rf_install("index_group"));
  int* p_index_group = INTEGER(index_group);

  // eval all the things
  for (R_xlen_t i_expr = 0; i_expr < n_expr; i_expr++) {
    SEXP quo = VECTOR_ELT(quosures, i_expr);
    SEXP name = STRING_ELT(names, i_expr);
    SEXP s_name = Rf_installChar(name);
    *p_index_expression = i_expr + 1;

    SEXP auto_name = STRING_ELT(auto_names, i_expr);
    SEXP s_auto_name = Rf_installChar(auto_name);

    *p_index_group = -1;
    SEXP hybrid_result = PROTECT(funs::eval_hybrid(quo, chops));
    if (hybrid_result != R_NilValue) {

      if (TYPEOF(hybrid_result) != VECSXP || XLENGTH(hybrid_result) != n_masks) {
        Rf_error("Malformed hybrid result, not a list");
      }

      SEXP ptype = Rf_getAttrib(hybrid_result, dplyr::symbols::ptype);
      if (ptype == R_NilValue) {
        Rf_error("Malformed hybrid result, needs ptype");
      }

      if (XLENGTH(name) == 0) {
        // if @ptype is a data frame, then auto splice as we go
        // this assumes all results exactly match the ptype
        if (Rf_inherits(ptype, "data.frame")) {
          R_xlen_t n_results = XLENGTH(ptype);

          SEXP result_names = Rf_getAttrib(ptype, R_NamesSymbol);
          SEXP result_symbols = Rf_allocVector(VECSXP, n_results);

          // only install once
          for (R_xlen_t i_result = 0; i_result < n_results; i_result++) {
            SET_VECTOR_ELT(result_symbols, i_result, Rf_installChar(STRING_ELT(result_names, i_result)));
          }

          for (R_xlen_t i_group = 0; i_group < n_masks; i_group++) {
            SEXP res_i = VECTOR_ELT(hybrid_result, i_group);
            SET_VECTOR_ELT(VECTOR_ELT(res, i_group), i_expr, res_i);
            SEXP mask = VECTOR_ELT(masks, i_group);

            for (R_xlen_t i_result = 0; i_result < n_results; i_result++) {
              Rf_defineVar(
                VECTOR_ELT(result_symbols, i_result),
                VECTOR_ELT(hybrid_result, i_result),
                mask
              );
            }
          }

        } else {
          // unnamed, but not a data frame, so use the deduced name
          for (R_xlen_t i_group = 0; i_group < n_masks; i_group++) {
            SEXP hybrid_res_i = VECTOR_ELT(hybrid_result, i_group);

            SEXP res_i = VECTOR_ELT(res, i_group);
            SET_VECTOR_ELT(res_i, i_expr, hybrid_res_i);

            SEXP names_res_i = Rf_getAttrib(res_i, R_NamesSymbol);
            SET_STRING_ELT(names_res_i, i_expr, auto_name);

            Rf_defineVar(s_auto_name, hybrid_res_i, VECTOR_ELT(masks, i_group));
          }
        }

      } else {
        // we have a proper name, so no auto splice or auto name use
        for (R_xlen_t i_group = 0; i_group < n_masks; i_group++) {
          SEXP res_i = VECTOR_ELT(hybrid_result, i_group);
          SET_VECTOR_ELT(VECTOR_ELT(res, i_group), i_expr, res_i);
          Rf_defineVar(s_name, res_i, VECTOR_ELT(masks, i_group));
        }
      }


    } else {
      for (R_xlen_t i_group = 0; i_group < n_masks; i_group++) {
        *p_index_group = i_group + 1;
        SEXP mask = VECTOR_ELT(masks, i_group);
        SEXP result = PROTECT(rlang::eval_tidy(quo, mask, caller_env));

        SET_VECTOR_ELT(VECTOR_ELT(res, i_group), i_expr, result);

        if (XLENGTH(name) == 0) {
          if (Rf_inherits(result, "data.frame")) {
            R_xlen_t n_columns = XLENGTH(result);
            SEXP names_columns = PROTECT(Rf_getAttrib(result, R_NamesSymbol));
            for (R_xlen_t i_column = 0; i_column < n_columns; i_column++) {
              SEXP name_i = Rf_installChar(STRING_ELT(names_columns, i_column));
              Rf_defineVar(name_i, VECTOR_ELT(result, i_column), mask);
            }
            UNPROTECT(1);
          } else {
            // this uses an auto name instead of ""
            SEXP names_res_i = Rf_getAttrib(VECTOR_ELT(res, i_group), R_NamesSymbol);
            SET_STRING_ELT(names_res_i, i_expr, auto_name);

            Rf_defineVar(s_auto_name, result, mask);
          }
        } else {
          Rf_defineVar(s_name, result, mask);
        }

        UNPROTECT(1);
      }
    }

  }

  UNPROTECT(2);
  return res;
}
