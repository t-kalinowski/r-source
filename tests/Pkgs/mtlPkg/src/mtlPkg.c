#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

static SEXP mtlPkg_add(SEXP x, SEXP y)
{
    double dx = asReal(x);
    double dy = asReal(y);
    return ScalarReal(dx + dy);
}

static SEXP mtlPkg_define_global(SEXP x)
{
    /* This intentionally uses the public symbol R_GlobalEnv so we can test that
       mtlapply() rejects worker-thread writes to globalenv(). */
    SEXP sym = install("mtl_test_var");
    defineVar(sym, x, R_GlobalEnv);
    return R_NilValue;
}

static SEXP mtlPkg_register_callable(SEXP dummy)
{
    R_RegisterCCallable("mtlPkg", "mtlPkg_add_callable", (DL_FUNC) &mtlPkg_add);
    return ScalarLogical(TRUE);
}

static SEXP mtlPkg_call_callable(SEXP x, SEXP y)
{
    DL_FUNC fun = R_GetCCallable("mtlPkg", "mtlPkg_add_callable");
    SEXP (*typed_fun)(SEXP, SEXP) = (SEXP (*)(SEXP, SEXP)) fun;
    return typed_fun(x, y);
}

static const R_CallMethodDef CallEntries[] = {
    {"mtlPkg_add", (DL_FUNC) &mtlPkg_add, 2},
    {"mtlPkg_define_global", (DL_FUNC) &mtlPkg_define_global, 1},
    {"mtlPkg_register_callable", (DL_FUNC) &mtlPkg_register_callable, 1},
    {"mtlPkg_call_callable", (DL_FUNC) &mtlPkg_call_callable, 2},
    {NULL, NULL, 0}
};

void R_init_mtlPkg(DllInfo *dll)
{
    R_registerRoutines(dll, NULL, CallEntries, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
}
