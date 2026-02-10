/*
 * Out-of-line helper for obtaining the current interpreter pointer.
 *
 * On macOS, accessing a thread_local symbol uses TLV machinery that may
 * compile to a call (e.g. __tlv_bootstrap). If this reference appears in an
 * always-inlined hot path, the compiler may schedule the TLV access even when
 * the runtime fast-path would return early.
 *
 * Keep TLS access out-of-line so the serial fast-path can compile without
 * pulling in TLV access sequences.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <Defn.h>

attribute_visible R_InterpreterState *R_mtl_interpreter_tls_or_main(void)
{
    return R_InterpreterTLS ? R_InterpreterTLS : R_InterpreterMain;
}
