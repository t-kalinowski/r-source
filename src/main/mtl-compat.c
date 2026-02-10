/*
 * Compatibility glue for the multi-heap / mtlapply experiment.
 *
 * Historically, internal shared objects (e.g. base packages built against
 * Defn.h) referenced the thread-local variable `R_Interpreter`.
 *
 * In the mtl experiment, `R_Interpreter` is now a macro that resolves to the
 * currently-active interpreter state without forcing TLS access on the serial
 * fast path. However, already-built internal shared objects still expect a
 * TLS symbol named `R_Interpreter` at runtime.
 *
 * Provide that symbol to keep existing internal shared objects loadable.
 * New code in the core uses the macro and should not rely on this variable.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <Defn.h>

/* Defn.h defines `R_Interpreter` as a macro in the mtl build. */
#undef R_Interpreter

attribute_visible R_THREAD_LOCAL R_InterpreterState *R_Interpreter = &R_Interpreter0;

attribute_hidden R_InterpreterState *R_mtl_set_compat_interpreter(R_InterpreterState *st)
{
    R_InterpreterState *old = R_Interpreter;
    R_Interpreter = st ? st : &R_Interpreter0;
    return old;
}
