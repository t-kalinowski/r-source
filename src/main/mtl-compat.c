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
#include <locale.h>
#include <dlfcn.h>
#include <string.h>

/* Defn.h defines `R_Interpreter` as a macro in the mtl build. */
#undef R_Interpreter
#undef R_BCNodeStackEnd
#undef R_BCNodeStackTop
#undef R_CStackLimit
#undef R_CStackStart
#undef R_GlobalContext
#undef R_OldCStackLimit
#undef R_PPStack
#undef R_PPStackTop
#undef R_ParseContext
#undef R_ParseContextLast
#undef R_ParseContextLine
#undef R_ParseError
#undef R_ParseErrorMsg
#undef R_Visible

attribute_visible R_THREAD_LOCAL R_InterpreterState *R_Interpreter = &R_Interpreter0;

/*
 * Export legacy interpreter globals expected by prebuilt binaries.
 *
 * These mirror fields now held in R_InterpreterState. We keep them in sync
 * on the serial path so binaries that resolve these symbols (e.g. IDE/runtime
 * integrations built against stock libR) remain loadable.
 */
attribute_visible R_bcstack_t *R_BCNodeStackEnd = NULL;
attribute_visible R_bcstack_t *R_BCNodeStackTop = NULL;
attribute_visible uintptr_t R_CStackLimit = 0;
attribute_visible uintptr_t R_CStackStart = 0;
attribute_visible void *R_GlobalContext = NULL;
attribute_visible uintptr_t R_OldCStackLimit = 0;
attribute_visible SEXP *R_PPStack = NULL;
attribute_visible int R_PPStackTop = 0;
attribute_visible char R_ParseContext[PARSE_CONTEXT_SIZE] = "";
attribute_visible int R_ParseContextLast = 0;
attribute_visible int R_ParseContextLine = 0;
attribute_visible int R_ParseError = 0;
attribute_visible char R_ParseErrorMsg[PARSE_ERROR_SIZE] = "";
attribute_visible Rboolean R_Visible = TRUE;

attribute_hidden void R_mtl_sync_compat_exports(void)
{
    R_InterpreterState *st;

    /* Avoid cross-thread writes while worker threads are active. */
    if (R_MTL_THREADING_ACTIVE)
	return;

    st = R_InterpreterMain ? R_InterpreterMain : &R_Interpreter0;
    R_BCNodeStackEnd = st->bcNodeStackEnd;
    R_BCNodeStackTop = st->bcNodeStackTop;
    R_CStackLimit = st->cStackLimit;
    R_CStackStart = st->cStackStart;
#ifdef R_USE_SIGNALS
    R_GlobalContext = st->globalContext;
#endif
    R_OldCStackLimit = st->oldCStackLimit;
    R_PPStack = st->ppStack;
    R_PPStackTop = st->ppStackTop;
    R_ParseContextLast = st->parseContextLast;
    R_ParseContextLine = st->parseContextLine;
    R_ParseError = st->parseError;
    R_Visible = st->visible;
    memcpy(R_ParseContext, st->parseContext, sizeof(R_ParseContext));
    memcpy(R_ParseErrorMsg, st->parseErrorMsg, sizeof(R_ParseErrorMsg));
}

attribute_hidden R_InterpreterState *R_mtl_set_compat_interpreter(R_InterpreterState *st)
{
    R_InterpreterState *old = R_Interpreter;
    R_Interpreter = st ? st : &R_Interpreter0;
    R_mtl_sync_compat_exports();
    return old;
}

#if defined(__APPLE__) && !defined(HAVE_X11)
#include <Rmodules/RX11.h>
/*
 * Keep symbol availability compatible with framework builds that include the
 * X11 module entrypoint even when configured --without-x.
 */
attribute_visible R_X11Routines *R_setX11Routines(R_X11Routines *routines)
{
    static R_X11Routines *ptr = NULL;
    R_X11Routines *old = ptr;
    ptr = routines;
    return old;
}
#endif

#if defined(__APPLE__) && defined(ENABLE_NLS)
/*
 * ABI compatibility for binary packages built against framework libR that
 * exports libintl_* symbols.  Homebrew-based builds link against libintl but
 * do not re-export these prefixed names from libR itself; packages that record
 * libR as their lookup image then fail to load with "Symbol not found:
 * _libintl_dgettext" (and siblings).
 *
 * Export thin wrappers in libR so existing binaries remain loadable.
 */
/* libintl headers may macro-redirect these names to libintl_* variants. */
#ifdef gettext
# undef gettext
#endif
#ifdef dgettext
# undef dgettext
#endif
#ifdef dcgettext
# undef dcgettext
#endif
#ifdef ngettext
# undef ngettext
#endif
#ifdef dngettext
# undef dngettext
#endif
#ifdef dcngettext
# undef dcngettext
#endif
#ifdef textdomain
# undef textdomain
#endif
#ifdef bindtextdomain
# undef bindtextdomain
#endif
#ifdef bind_textdomain_codeset
# undef bind_textdomain_codeset
#endif
#ifdef setlocale
# undef setlocale
#endif
#ifdef newlocale
# undef newlocale
#endif

extern char *gettext(const char *);
extern char *dgettext(const char *, const char *);
extern char *dcgettext(const char *, const char *, int);
extern char *ngettext(const char *, const char *, unsigned long int);
extern char *dngettext(const char *, const char *, const char *, unsigned long int);
extern char *dcngettext(const char *, const char *, const char *, unsigned long int, int);
extern char *textdomain(const char *);
extern char *bindtextdomain(const char *, const char *);
extern char *bind_textdomain_codeset(const char *, const char *);
extern char *setlocale(int, const char *);
extern locale_t newlocale(int, const char *, locale_t);

attribute_visible char *libintl_gettext(const char *msgid)
{
    return gettext(msgid);
}

attribute_visible char *libintl_dgettext(const char *domainname, const char *msgid)
{
    return dgettext(domainname, msgid);
}

attribute_visible char *libintl_dcgettext(const char *domainname, const char *msgid, int category)
{
    return dcgettext(domainname, msgid, category);
}

attribute_visible char *libintl_ngettext(const char *msgid1, const char *msgid2,
					 unsigned long int n)
{
    return ngettext(msgid1, msgid2, n);
}

attribute_visible char *libintl_dngettext(const char *domainname, const char *msgid1,
					  const char *msgid2, unsigned long int n)
{
    return dngettext(domainname, msgid1, msgid2, n);
}

attribute_visible char *libintl_dcngettext(const char *domainname, const char *msgid1,
					   const char *msgid2, unsigned long int n,
					   int category)
{
    return dcngettext(domainname, msgid1, msgid2, n, category);
}

attribute_visible char *libintl_textdomain(const char *domainname)
{
    return textdomain(domainname);
}

attribute_visible char *libintl_bindtextdomain(const char *domainname, const char *dirname)
{
    return bindtextdomain(domainname, dirname);
}

attribute_visible char *libintl_bind_textdomain_codeset(const char *domainname,
							const char *codeset)
{
    return bind_textdomain_codeset(domainname, codeset);
}

attribute_visible char *libintl_setlocale(int category, const char *locale)
{
    return setlocale(category, locale);
}

attribute_visible locale_t libintl_newlocale(int category_mask, const char *name, locale_t base)
{
    return newlocale(category_mask, name, base);
}

attribute_visible void libintl_set_relocation_prefix(const char *orig_prefix,
						     const char *curr_prefix)
{
    typedef void (*set_reloc_t)(const char *, const char *);
    static set_reloc_t p = NULL;
    if (p == NULL)
	p = (set_reloc_t) dlsym(RTLD_NEXT, "libintl_set_relocation_prefix");
    if (p != NULL)
	p(orig_prefix, curr_prefix);
}
#endif
