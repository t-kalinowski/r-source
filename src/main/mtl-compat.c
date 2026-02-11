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

/* Defn.h defines `R_Interpreter` as a macro in the mtl build. */
#undef R_Interpreter

attribute_visible R_THREAD_LOCAL R_InterpreterState *R_Interpreter = &R_Interpreter0;

attribute_hidden R_InterpreterState *R_mtl_set_compat_interpreter(R_InterpreterState *st)
{
    R_InterpreterState *old = R_Interpreter;
    R_Interpreter = st ? st : &R_Interpreter0;
    return old;
}

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
