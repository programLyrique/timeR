/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1997--2025  The R Core Team
 *  Copyright (C) 2003	      The R Foundation
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, a copy is available at
 *  https://www.R-project.org/Licenses/
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#define R_USE_SIGNALS 1
#include <Defn.h>
#include <Internal.h>
#include <ctype.h> /* for tolower */
#include <string.h>
#include <errno.h>

#include <Rmath.h>

#include "timeR.h"


#ifndef max
#define max(a, b) ((a > b)?(a):(b))
#endif

/* Was 'name' prior to 2.13.0, then .NAME, but checked as
   'name' up to 2.15.1. */
static void check1arg2(SEXP arg, SEXP call, const char *formal)
{
    if (TAG(arg) == R_NilValue) return;
    errorcall(call, "the first argument should not be named");
 }



/* These are set during the first call to do_dotCode() below. */

static SEXP NaokSymbol = NULL;
static SEXP DupSymbol = NULL;
static SEXP PkgSymbol = NULL;
static SEXP EncSymbol = NULL;
static SEXP CSingSymbol = NULL;

#include <Rdynpriv.h>
// Odd: 'type' is really this enum
enum {NOT_DEFINED, FILENAME, DLL_HANDLE, R_OBJECT};
typedef struct {
    char DLLname[R_PATH_MAX];
    HINSTANCE dll;
    SEXP  obj;
    int type;
} DllReference;

/* Maximum length of entry-point name, including nul terminator */
#define MaxSymbolBytes 1024

/* Maximum number of args to .C, .Fortran and .Call */
#define MAX_ARGS 65

/* This looks up entry points in DLLs in a platform specific way. */
static DL_FUNC
R_FindNativeSymbolFromDLL(char *name, DllReference *dll,
			  R_RegisteredNativeSymbol *symbol, SEXP env);

static SEXP naokfind(SEXP args, int * len, int *naok, DllReference *dll);
static SEXP pkgtrim(SEXP args, DllReference *dll);

static R_INLINE bool isNativeSymbolInfo(SEXP op)
{
    /* was: inherits(op, "NativeSymbolInfo")
     * inherits() is slow because of string comparisons, so use
     * structural check instead. */
    return (TYPEOF(op) == VECSXP &&
	    LENGTH(op) >= 2 &&
	    TYPEOF(VECTOR_ELT(op, 1)) == EXTPTRSXP);
}

/*
  Called from resolveNativeRoutine (and itself).

  Checks whether the specified object correctly identifies a native routine.
  op is the supplied value for .NAME.  This can be
   a) a string (when this does nothing).
   b) an external pointer giving the address of the routine
      (e.g. getNativeSymbolInfo("foo")$address)
   c) or a NativeSymbolInfo itself  (e.g. getNativeSymbolInfo("foo"))

   It copies the symbol name to buf.

   NB: in the last two cases it sets fun and symbol as well!
 */
static void
checkValidSymbolId(SEXP op, SEXP call, DL_FUNC *fun,
		   R_RegisteredNativeSymbol *symbol, char *buf)
{
    if (isValidString(op)) return;

    if(TYPEOF(op) == EXTPTRSXP) {
	static SEXP native_symbol = NULL;
	static SEXP registered_native_symbol = NULL;
	if (native_symbol == NULL) {
	    native_symbol = install("native symbol");
	    registered_native_symbol = install("registered native symbol");
	}
	char *p = NULL;
	if(R_ExternalPtrTag(op) == native_symbol)
	   *fun = R_ExternalPtrAddrFn(op);
	else if(R_ExternalPtrTag(op) == registered_native_symbol) {
	   R_RegisteredNativeSymbol *tmp;
	   tmp = (R_RegisteredNativeSymbol *) R_ExternalPtrAddr(op);
	   if(tmp) {
	      if(symbol->type != R_ANY_SYM && symbol->type != tmp->type)
		 errorcall(call, _("NULL value passed as symbol address"));
		/* Check the type of the symbol. */
	      switch(symbol->type) {
	      case R_C_SYM:
		  *fun = tmp->symbol.c->fun;
		  p = tmp->symbol.c->name;
		  break;
	      case R_CALL_SYM:
		  *fun = tmp->symbol.call->fun;
		  p = tmp->symbol.call->name;
		  break;
	      case R_FORTRAN_SYM:
		  *fun = tmp->symbol.fortran->fun;
		  p = tmp->symbol.fortran->name;
		  break;
	      case R_EXTERNAL_SYM:
		  *fun = tmp->symbol.external->fun;
		  p = tmp->symbol.external->name;
		  break;
	      default:
		 /* Something unintended has happened if we get here. */
		  errorcall(call, _("Unimplemented type %d in createRSymbolObject"),
			    symbol->type);
		  break;
	      }
	      *symbol = *tmp;
	   }
	}
	/* This is illegal C */
	if(*fun == NULL)
	    errorcall(call, _("NULL value passed as symbol address"));

	/* copy the symbol name. */
	if (p && buf) {
	    if (strlen(p) >= MaxSymbolBytes)
		error(_("symbol '%s' is too long"), p);
	    memcpy(buf, p, strlen(p)+1);
	}

	return;
    }
    else if(isNativeSymbolInfo(op)) {
	checkValidSymbolId(VECTOR_ELT(op, 1), call, fun, symbol, buf);
	return;
    }

    errorcall(call,
      _("first argument must be a string (of length 1) or native symbol reference"));
    return; /* not reached */
}

attribute_hidden
DL_FUNC R_dotCallFn(SEXP op, SEXP call, int nargs) {
    R_RegisteredNativeSymbol symbol = {R_CALL_SYM, {NULL}, NULL};
    DL_FUNC fun = NULL;
    checkValidSymbolId(op, call, &fun, &symbol, NULL);
    /* should check arg count here as well */
    return fun;
}

/*
  This is the routine that is called by do_dotCode, do_dotcall and
  do_External to find the DL_FUNC to invoke. It handles processing the
  arguments for the PACKAGE argument, if present, and also takes care
  of the cases where we are given a NativeSymbolInfo object, an
  address directly, and if the DLL is specified. If no PACKAGE is
  provided, we check whether the calling function is in a namespace
  and look there.
*/

static SEXP
resolveNativeRoutine(SEXP args, DL_FUNC *fun,
		     R_RegisteredNativeSymbol *symbol, char *buf,
		     int *nargs, int *naok, SEXP call, SEXP env)
{
    SEXP op;
    const char *p; char *q;
    DllReference dll;
    /* This is used as shorthand for 'all' in R_FindSymbol, but
       should never be supplied */
    strcpy(dll.DLLname, "");
    dll.dll = NULL; dll.obj = NULL; dll.type = NOT_DEFINED;

    op = CAR(args);  // value of .NAME =
    /* NB, this sets fun, symbol and buf and is not just a check! */
    checkValidSymbolId(op, call, fun, symbol, buf);

    /* The following code modifies the argument list */
    /* We know this is ok because do_dotCode is entered */
    /* with its arguments evaluated. */

    if(symbol->type == R_C_SYM || symbol->type == R_FORTRAN_SYM) {
	/* And that also looks for PACKAGE = */
	args = naokfind(CDR(args), nargs, naok, &dll);
	if(*naok == NA_LOGICAL)
	    errorcall(call, _("invalid '%s' value"), "naok");
	if(*nargs > MAX_ARGS)
	    errorcall(call, _("too many arguments in foreign function call"));
    } else {
	/* This has the side effect of setting dll.type if a PACKAGE=
	   argument if found, but it will only be used if a string was
	   passed in  */
	args = pkgtrim(args, &dll);
    }

    /* We were given a symbol (or an address), so we are done. */
    if (*fun) return args;

    if (dll.type == FILENAME && !strlen(dll.DLLname))
	errorcall(call, _("PACKAGE = \"\" is invalid"));

    // find if we were called from a namespace
    SEXP env2 = ENCLOS(env);
    const char *ns = "";
    if(R_IsNamespaceEnv(env2))
	ns = CHAR(STRING_ELT(R_NamespaceEnvSpec(env2), 0));
    else env2 = R_NilValue;

#ifdef CHECK_CROSS_USAGE
    if (dll.type == FILENAME && strcmp(dll.DLLname, "base")) {
	if(strlen(ns) && strcmp(dll.DLLname, ns) &&
	   !(streql(dll.DLLname, "BioC_graph") && streql(ns, "graph")))
	    warningcall(call,
			"using PACKAGE = \"%s\" from namespace '%s'",
			dll.DLLname, ns);
    }
#endif

    /* Make up the load symbol */
    if(TYPEOF(op) == STRSXP) {
	const void *vmax = vmaxget();
	p = translateChar(STRING_ELT(op, 0));
	if(strlen(p) >= MaxSymbolBytes)
	    error(_("symbol '%s' is too long"), p);
	q = buf;
	while ((*q = *p) != '\0') {
	    if(symbol->type == R_FORTRAN_SYM) *q = (char) tolower(*q);
	    p++;
	    q++;
	}
	vmaxset(vmax);
    }

    if(dll.type != FILENAME && strlen(ns)) {
	/* no PACKAGE= arg, so see if we can identify a DLL
	   from the namespace defining the function */
	*fun = R_FindNativeSymbolFromDLL(buf, &dll, symbol, env2);
	if (*fun) return args;
	errorcall(call, "\"%s\" not resolved from current namespace (%s)",
		  buf, ns);
    }

    /* NB: the actual conversion to the symbol is done in
       R_dlsym in Rdynload.c.  That prepends an underscore (usually),
       and may append one or more underscores.
    */

    *fun = R_FindSymbol(buf, dll.DLLname, symbol);
    if (*fun) return args;

    /* so we've failed and bail out */
    if(strlen(dll.DLLname)) {
	switch(symbol->type) {
	case R_C_SYM:
	    errorcall(call,
		      _("\"%s\" not available for %s() for package \"%s\""),
		      buf, ".C", dll.DLLname);
	    break;
	case R_FORTRAN_SYM:
	    errorcall(call,
		      _("\"%s\" not available for %s() for package \"%s\""),
		      buf, ".Fortran", dll.DLLname);
	    break;
	case R_CALL_SYM:
	    errorcall(call,
		      _("\"%s\" not available for %s() for package \"%s\""),
		      buf, ".Call", dll.DLLname);
	    break;
	case R_EXTERNAL_SYM:
	    errorcall(call,
		      _("\"%s\" not available for %s() for package \"%s\""),
		      buf, ".External", dll.DLLname);
	    break;
	case R_ANY_SYM:
	    errorcall(call,
		      _("%s symbol name \"%s\" not in DLL for package \"%s\""),
		      "C/Fortran", buf, dll.DLLname);
	    break;
	}
    } else
	errorcall(call, _("%s symbol name \"%s\" not in load table"),
		  symbol->type == R_FORTRAN_SYM ? "Fortran" : "C", buf);

    return args; /* -Wall */
}


static bool
checkNativeType(int targetType, int actualType)
{
    if(targetType > 0) {
	if(targetType == INTSXP || targetType == LGLSXP) {
	    return(actualType == INTSXP || actualType == LGLSXP);
	}
	return(targetType == actualType);
    }

    return(true);
}


static bool
comparePrimitiveTypes(R_NativePrimitiveArgType type, SEXP s)
{
   if(type == ANYSXP || TYPEOF(s) == type)
      return(true);

   if(type == SINGLESXP)
      return(asLogical(getAttrib(s, install("Csingle"))) == TRUE);

   return(false);
}


/* Foreign Function Interface.  This code allows a user to call C */
/* or Fortran code which is either statically or dynamically linked. */

/* NB: this leaves NAOK and DUP arguments on the list */

/* find NAOK and DUP, find and remove PACKAGE */
static SEXP naokfind(SEXP args, int * len, int *naok, DllReference *dll)
{
    SEXP s, prev;
    int nargs=0, naokused=0, dupused=0, pkgused=0;
    const char *p;

    *naok = 0;
    *len = 0;
    for(s = args, prev=args; s != R_NilValue;) {
	if(TAG(s) == NaokSymbol) {
	    *naok = asLogical(CAR(s));
	    if(naokused++ == 1) warning(_("'%s' used more than once"), "NAOK");
	} else if(TAG(s) == DupSymbol) {
	    if(dupused++ == 1) warning(_("'%s' used more than once"), "DUP");
	} else if(TAG(s) == PkgSymbol) {
	    dll->obj = CAR(s);  // really?
	    if(TYPEOF(CAR(s)) == STRSXP) {
		p = translateChar(STRING_ELT(CAR(s), 0));
		if(strlen(p) > R_PATH_MAX - 1)
		    error(_("DLL name is too long"));
		dll->type = FILENAME;
		strcpy(dll->DLLname, p);
		if(pkgused++ > 1)
		    warning(_("'%s' used more than once"), "PACKAGE");
		/* More generally, this should allow us to process
		   any additional arguments and not insist that PACKAGE
		   be the last argument.
		*/
	    } else {
		/* Have a DLL object, which is not something documented .... */
		if(TYPEOF(CAR(s)) == EXTPTRSXP) {
		    dll->dll = (HINSTANCE) R_ExternalPtrAddr(CAR(s));
		    dll->type = DLL_HANDLE;
		} else if(TYPEOF(CAR(s)) == VECSXP) {
		    dll->type = R_OBJECT;
		    dll->obj = s;
		    strcpy(dll->DLLname,
			   translateChar(STRING_ELT(VECTOR_ELT(CAR(s), 1), 0)));
		    dll->dll = (HINSTANCE) R_ExternalPtrAddr(VECTOR_ELT(s, 4));
		} else
		    error("incorrect type (%s) of PACKAGE argument\n",
			  R_typeToChar(CAR(s)));
	    }
	} else {
	    nargs++;
	    prev = s;
	    s = CDR(s);
	    continue;
	}
	if(s == args)
	    args = s = CDR(s);
	else
	    SETCDR(prev, s = CDR(s));
    }
    *len = nargs;
    return args;
}

static void setDLLname(SEXP s, char *DLLname)
{
    SEXP ss = CAR(s);
    const char *name;

    if(TYPEOF(ss) != STRSXP || length(ss) != 1)
	error(_("PACKAGE argument must be a single character string"));
    name = translateChar(STRING_ELT(ss, 0));
    /* allow the package: form of the name, as returned by find */
    if(strncmp(name, "package:", 8) == 0)
	name += 8;
    if(strlen(name) > R_PATH_MAX - 1)
	error(_("PACKAGE argument is too long"));
    strcpy(DLLname, name);
}

static SEXP pkgtrim(SEXP args, DllReference *dll)
{
    SEXP s, ss;
    int pkgused = 0;

    if (PkgSymbol == NULL) PkgSymbol = install("PACKAGE");

    for(s = args ; s != R_NilValue;) {
	ss = CDR(s);
	/* Look for PACKAGE=. We look at the next arg, unless
	   this is the last one (which will only happen for one arg),
	   and remove it */
	if(ss == R_NilValue && TAG(s) == PkgSymbol) {
	    if(pkgused++ == 1)
		warning(_("'%s' used more than once"), "PACKAGE");
	    setDLLname(s, dll->DLLname);
	    dll->type = FILENAME;
	    return R_NilValue;
	}
	if(TAG(ss) == PkgSymbol) {
	    if(pkgused++ == 1)
		warning(_("'%s' used more than once"), "PACKAGE");
	    setDLLname(ss, dll->DLLname);
	    dll->type = FILENAME;
	    SETCDR(s, CDR(ss));
	}
	s = CDR(s);
    }
    return args;
}

static SEXP enctrim(SEXP args)
{
    SEXP s, ss;

    for(s = args ; s != R_NilValue;) {
	ss = CDR(s);
	/* Look for ENCODING=. We look at the next arg, unless
	   this is the last one (which will only happen for one arg),
	   and remove it */
	if(ss == R_NilValue && TAG(s) == EncSymbol) {
	    warning("ENCODING is defunct and will be ignored");
	    return R_NilValue;
	}
	if(TAG(ss) == EncSymbol) {
	    warning("ENCODING is defunct and will be ignored");
	    SETCDR(s, CDR(ss));
	}
	s = CDR(s);
    }
    return args;
}



attribute_hidden SEXP do_isloaded(SEXP call, SEXP op, SEXP args, SEXP env)
{
    const char *sym, *type="", *pkg = "";
    int val = 1, nargs = length(args);
    R_RegisteredNativeSymbol symbol = {R_ANY_SYM, {NULL}, NULL};

    if (nargs < 1) error(_("no arguments supplied"));
    if (nargs > 3) error(_("too many arguments"));

    if(!isValidString(CAR(args)))
	error(_("invalid '%s' argument"), "symbol");
    sym = translateChar(STRING_ELT(CAR(args), 0));
    if(nargs >= 2) {
	if(!isValidString(CADR(args)))
	    error(_("invalid '%s' argument"), "PACKAGE");
	pkg = translateChar(STRING_ELT(CADR(args), 0));
    }
    if(nargs >= 3) {
	if(!isValidString(CADDR(args)))
	    error(_("invalid '%s' argument"), "type");
	type = CHAR(STRING_ELT(CADDR(args), 0)); /* ASCII */
	if(strcmp(type, "C") == 0) symbol.type = R_C_SYM;
	else if(strcmp(type, "Fortran") == 0) symbol.type = R_FORTRAN_SYM;
	else if(strcmp(type, "Call") == 0) symbol.type = R_CALL_SYM;
	else if(strcmp(type, "External") == 0) symbol.type = R_EXTERNAL_SYM;
    }
    if(!(R_FindSymbol(sym, pkg, &symbol))) val = 0;
    return ScalarLogical(val);
}

/*   Call dynamically loaded "internal" functions.
     Original code by Jean Meloche <jean@stat.ubc.ca> */

typedef SEXP (*R_ExternalRoutine)(SEXP);
typedef SEXP (*R_ExternalRoutine2)(SEXP, SEXP, SEXP, SEXP);

static SEXP check_retval(SEXP call, SEXP val)
{
    static int inited = FALSE;
    static int check = FALSE;

    if (! inited) {
	inited = true;
	const char *p = getenv("_R_CHECK_DOTCODE_RETVAL_");
	if (p != NULL && StringTrue(p))
	    check = true;
    }

    if (check) {
	if (val < (SEXP) 16)
	    errorcall(call, "WEIRD RETURN VALUE: %p", (void *)val);
    }
    else if (val == NULL) {
	warningcall(call, "converting NULL pointer to R NULL");
	val = R_NilValue;
    }

    return val;
}

attribute_hidden SEXP do_External(SEXP call, SEXP op, SEXP args, SEXP env)
{
    BEGIN_TIMER(TR_dotExternalFull);
	DL_FUNC ofun = NULL;
    SEXP retval;
    R_RegisteredNativeSymbol symbol = {R_EXTERNAL_SYM, {NULL}, NULL};
    const void *vmax = vmaxget();
    char buf[MaxSymbolBytes];

    if (length(args) < 1) errorcall(call, _("'.NAME' is missing"));
    check1arg2(args, call, ".NAME");
    args = resolveNativeRoutine(args, &ofun, &symbol, buf, NULL, NULL,
				call, env);

    if(symbol.symbol.external && symbol.symbol.external->numArgs > -1) {
	int nargs = length(args) - 1;
	if(symbol.symbol.external->numArgs != nargs)
	    errorcall(call,
		      _("Incorrect number of arguments (%d), expecting %d for '%s'"),
		      nargs, symbol.symbol.external->numArgs, buf);
    }

    /* args is escaping into user C code and might get captured, so
       make sure it is reference counting. */
    R_args_enable_refcnt(args);

    if (PRIMVAL(op) == 1) {
	R_ExternalRoutine2 fun = (R_ExternalRoutine2) ofun;
	BEGIN_TIMER(TR_dotExternal);
	BEGIN_EXTERNAL_TIMER(buf, ofun);
	retval = fun(call, op, args, env);
	END_EXTERNAL_TIMER();
	END_TIMER(TR_dotExternal);
    } else {
	R_ExternalRoutine fun = (R_ExternalRoutine) ofun;
	BEGIN_TIMER(TR_dotExternal);
	BEGIN_EXTERNAL_TIMER(buf, ofun);
	retval = fun(args);
	END_EXTERNAL_TIMER();
	END_TIMER(TR_dotExternal);
    }

    R_try_clear_args_refcnt(args);

    vmaxset(vmax);
	END_TIMER(TR_dotExternalFull);
    return check_retval(call, retval);
}

#define R_FUNTYPES(R, N, A)                                                   \
typedef R (*FUN##N##0)(void);                                                 \
typedef R (*FUN##N##1)(A);                                                    \
typedef R (*FUN##N##2)(A, A);                                                 \
typedef R (*FUN##N##3)(A, A, A);                                              \
typedef R (*FUN##N##4)(A, A, A, A);                                           \
typedef R (*FUN##N##5)(A, A, A, A, A);                                        \
typedef R (*FUN##N##6)(A, A, A, A, A, A);                                     \
typedef R (*FUN##N##7)(A, A, A, A, A, A, A);                                  \
typedef R (*FUN##N##8)(A, A, A, A, A, A, A, A);                               \
typedef R (*FUN##N##9)(A, A, A, A, A, A, A, A, A);                            \
typedef R (*FUN##N##10)(A, A, A, A, A, A, A, A, A, A);                        \
typedef R (*FUN##N##11)(A, A, A, A, A, A, A, A, A, A, A);                     \
typedef R (*FUN##N##12)(A, A, A, A, A, A, A, A, A, A, A, A);                  \
typedef R (*FUN##N##13)(A, A, A, A, A, A, A, A, A, A, A, A, A);               \
typedef R (*FUN##N##14)(A, A, A, A, A, A, A, A, A, A, A, A, A, A);            \
typedef R (*FUN##N##15)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A);         \
typedef R (*FUN##N##16)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A);      \
typedef R (*FUN##N##17)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A);   \
typedef R (*FUN##N##18)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A);\
typedef R (*FUN##N##19)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A);                                                   \
typedef R (*FUN##N##20)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A);                                                \
typedef R (*FUN##N##21)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A);                                             \
typedef R (*FUN##N##22)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A);                                          \
typedef R (*FUN##N##23)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A);                                       \
typedef R (*FUN##N##24)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A);                                    \
typedef R (*FUN##N##25)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A);                                 \
typedef R (*FUN##N##26)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A);                              \
typedef R (*FUN##N##27)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A);                           \
typedef R (*FUN##N##28)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A);                        \
typedef R (*FUN##N##29)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A);                     \
typedef R (*FUN##N##30)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A);                  \
typedef R (*FUN##N##31)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A);               \
typedef R (*FUN##N##32)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A);            \
typedef R (*FUN##N##33)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A);         \
typedef R (*FUN##N##34)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A);      \
typedef R (*FUN##N##35)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A);   \
typedef R (*FUN##N##36)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A);\
typedef R (*FUN##N##37)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A);                                                   \
typedef R (*FUN##N##38)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A);                                                \
typedef R (*FUN##N##39)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A);                                             \
typedef R (*FUN##N##40)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A);                                          \
typedef R (*FUN##N##41)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A);                                       \
typedef R (*FUN##N##42)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A);                                    \
typedef R (*FUN##N##43)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A);                                 \
typedef R (*FUN##N##44)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A);                              \
typedef R (*FUN##N##45)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A);                           \
typedef R (*FUN##N##46)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A);                        \
typedef R (*FUN##N##47)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A);                     \
typedef R (*FUN##N##48)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A);                  \
typedef R (*FUN##N##49)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A);               \
typedef R (*FUN##N##50)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A);            \
typedef R (*FUN##N##51)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A);         \
typedef R (*FUN##N##52)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A);      \
typedef R (*FUN##N##53)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A);   \
typedef R (*FUN##N##54)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A);\
typedef R (*FUN##N##55)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A);                                                   \
typedef R (*FUN##N##56)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A);                                                \
typedef R (*FUN##N##57)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A);                                             \
typedef R (*FUN##N##58)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A);                                          \
typedef R (*FUN##N##59)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A);                                       \
typedef R (*FUN##N##60)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A);                                    \
typedef R (*FUN##N##61)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A);                                 \
typedef R (*FUN##N##62)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A);                              \
typedef R (*FUN##N##63)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A);                           \
typedef R (*FUN##N##64)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A);                        \
typedef R (*FUN##N##65)(A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, \
                        A, A, A, A, A, A, A, A, A, A, A);

/* typedef SEXP (*FUNS1)(SEXP); */
R_FUNTYPES(SEXP, S, SEXP)

/* typedef void (*FUNV1)(void *); */
R_FUNTYPES(void, V, void *)

#ifdef __cplusplus
typedef SEXP (*VarFun)(...);
#else
typedef DL_FUNC VarFun;
#endif

attribute_hidden SEXP R_doDotCall(DL_FUNC ofun, int nargs, SEXP *cargs,
				  SEXP call) {
	BEGIN_TIMER(TR_RdoDotCall);
    SEXP retval = R_NilValue;	/* -Wall */
	VarFun fun = NULL;
	fun = (VarFun) ofun;
    BEGIN_EXTERNAL_TIMER(buf, ofun);
    switch (nargs) {
    case 0:
	retval = ((FUNS0)fun)();
	break;
    case 1:
	retval = ((FUNS1)fun)(cargs[0]);
	break;
    case 2:
	retval = ((FUNS2)fun)(cargs[0], cargs[1]);
	break;
    case 3:
	retval = ((FUNS3)fun)(cargs[0], cargs[1], cargs[2]);
	break;
    case 4:
	retval = ((FUNS4)fun)(cargs[0], cargs[1], cargs[2], cargs[3]);
	break;
    case 5:
	retval = ((FUNS5)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4]);
	break;
    case 6:
	retval = ((FUNS6)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5]);
	break;
    case 7:
	retval = ((FUNS7)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6]);
	break;
    case 8:
	retval = ((FUNS8)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7]);
	break;
    case 9:
	retval = ((FUNS9)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8]);
	break;
    case 10:
	retval = ((FUNS10)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9]);
	break;
    case 11:
	retval = ((FUNS11)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10]);
	break;
    case 12:
	retval = ((FUNS12)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11]);
	break;
    case 13:
	retval = ((FUNS13)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12]);
	break;
    case 14:
	retval = ((FUNS14)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13]);
	break;
    case 15:
	retval = ((FUNS15)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14]);
	break;
    case 16:
	retval = ((FUNS16)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15]);
	break;
    case 17:
	retval = ((FUNS17)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16]);
	break;
    case 18:
	retval = ((FUNS18)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17]);
	break;
    case 19:
	retval = ((FUNS19)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18]);
	break;
    case 20:
	retval = ((FUNS20)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19]);
	break;
    case 21:
	retval = ((FUNS21)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20]);
	break;
    case 22:
	retval = ((FUNS22)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21]);
	break;
    case 23:
	retval = ((FUNS23)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22]);
	break;
    case 24:
	retval = ((FUNS24)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23]);
	break;
    case 25:
	retval = ((FUNS25)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24]);
	break;
    case 26:
	retval = ((FUNS26)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25]);
	break;
    case 27:
	retval = ((FUNS27)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26]);
	break;
    case 28:
	retval = ((FUNS28)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27]);
	break;
    case 29:
	retval = ((FUNS29)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28]);
	break;
    case 30:
	retval = ((FUNS30)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29]);
	break;
    case 31:
	retval = ((FUNS31)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30]);
	break;
    case 32:
	retval = ((FUNS32)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31]);
	break;
    case 33:
	retval = ((FUNS33)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32]);
	break;
    case 34:
	retval = ((FUNS34)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33]);
	break;
    case 35:
	retval = ((FUNS35)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34]);
	break;
    case 36:
	retval = ((FUNS36)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35]);
	break;
    case 37:
	retval = ((FUNS37)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36]);
	break;
    case 38:
	retval = ((FUNS38)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37]);
	break;
    case 39:
	retval = ((FUNS39)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38]);
	break;
    case 40:
	retval = ((FUNS40)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39]);
	break;
    case 41:
	retval = ((FUNS41)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40]);
	break;
    case 42:
	retval = ((FUNS42)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41]);
	break;
    case 43:
	retval = ((FUNS43)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42]);
	break;
    case 44:
	retval = ((FUNS44)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43]);
	break;
    case 45:
	retval = ((FUNS45)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44]);
	break;
    case 46:
	retval = ((FUNS46)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45]);
	break;
    case 47:
	retval = ((FUNS47)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46]);
	break;
    case 48:
	retval = ((FUNS48)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47]);
	break;
    case 49:
	retval = ((FUNS49)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48]);
	break;
    case 50:
	retval = ((FUNS50)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49]);
	break;
    case 51:
	retval = ((FUNS51)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50]);
	break;
    case 52:
	retval = ((FUNS52)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51]);
	break;
    case 53:
	retval = ((FUNS53)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52]);
	break;
    case 54:
	retval = ((FUNS54)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53]);
	break;
    case 55:
	retval = ((FUNS55)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54]);
	break;
    case 56:
	retval = ((FUNS56)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55]);
	break;
    case 57:
	retval = ((FUNS57)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56]);
	break;
    case 58:
	retval = ((FUNS58)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57]);
	break;
    case 59:
	retval = ((FUNS59)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58]);
	break;
    case 60:
	retval = ((FUNS60)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58], cargs[59]);
	break;
    case 61:
	retval = ((FUNS61)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58], cargs[59],
	    cargs[60]);
	break;
    case 62:
	retval = ((FUNS62)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58], cargs[59],
	    cargs[60], cargs[61]);
	break;
    case 63:
	retval = ((FUNS63)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58], cargs[59],
	    cargs[60], cargs[61], cargs[62]);
	break;
    case 64:
	retval = ((FUNS64)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58], cargs[59],
	    cargs[60], cargs[61], cargs[62], cargs[63]);
	break;
    case 65:
	retval = ((FUNS65)fun)(
	    cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58], cargs[59],
	    cargs[60], cargs[61], cargs[62], cargs[63], cargs[64]);
	break;
    default:
	errorcall(call, _("too many arguments, sorry"));
    }
	END_EXTERNAL_TIMER();
    END_TIMER(TR_RdoDotCall);
    return check_retval(call, retval);
}

/* .Call(name, <args>) */
attribute_hidden SEXP do_dotcall(SEXP call, SEXP op, SEXP args, SEXP env)
{
	BEGIN_TIMER(TR_doDotCall);
    DL_FUNC ofun = NULL;
    SEXP retval, cargs[MAX_ARGS], pargs;
    R_RegisteredNativeSymbol symbol = {R_CALL_SYM, {NULL}, NULL};

    int nargs;
    const void *vmax = vmaxget();
    char buf[MaxSymbolBytes];
    int nprotect = 0;

    if (length(args) < 1) errorcall(call, _("'.NAME' is missing"));
    check1arg2(args, call, ".NAME");

    args = resolveNativeRoutine(args, &ofun, &symbol, buf, NULL, NULL, call, env);
    args = CDR(args);

    for(nargs = 0, pargs = args ; pargs != R_NilValue; pargs = CDR(pargs)) {
	if (nargs == MAX_ARGS)
	    errorcall(call, _("too many arguments in foreign function call"));
	cargs[nargs] = CAR(pargs);
	nargs++;
    }
    if(symbol.symbol.call && symbol.symbol.call->numArgs > -1) {
	if(symbol.symbol.call->numArgs != nargs)
	    errorcall(call,
		      _("Incorrect number of arguments (%d), expecting %d for '%s'"),
		      nargs, symbol.symbol.call->numArgs, buf);
    }

    if (R_check_constants < 4)
	retval = R_doDotCall(ofun, nargs, cargs, call);
    else {
	SEXP *cargscp = (SEXP *) R_alloc(nargs, sizeof(SEXP));
	int i;
	for(i = 0; i < nargs; i++) {
	    cargscp[i] = PROTECT(duplicate(cargs[i]));
	    nprotect++;
	}
	retval = PROTECT(R_doDotCall(ofun, nargs, cargs, call));
	nprotect++;
	bool constsOK = true;
	for(i = 0; constsOK && i < nargs; i++)
	    /* 39: not numerical comparison, not single NA, not attributes as
               set, do ignore byte-code, do ignore environments of closures,
               not ignore srcref

               srcref is not ignored because ignoring it is expensive
               (it triggers duplication)
	    */
            if (!R_compute_identical(cargs[i], cargscp[i], 39)
		    && !R_checkConstants(FALSE))
		constsOK = false;
	if (!constsOK) {
	    REprintf("ERROR: detected compiler constant(s) modification after"
		" .Call invocation of function %s from library %s (%s).\n",
		buf,
		symbol.dll ? symbol.dll->name : "unknown",
		symbol.dll ? symbol.dll->path : "unknown");
	    for(i = 0; i < nargs; i++)
		if (!R_compute_identical(cargs[i], cargscp[i], 39))
		    REprintf("NOTE: .Call function %s modified its argument"
			" (number %d, type %s, length %d)\n",
			buf,
			i + 1,
			CHAR(type2str(TYPEOF(cargscp[i]))),
			length(cargscp[i])
		    );
	    R_Suicide("compiler constants were modified (in .Call?)!\n");
	}
	UNPROTECT(nprotect);
    }
    vmaxset(vmax);
	END_TIMER(TR_doDotCall);
    return retval;
}

/*  Call dynamically loaded "internal" graphics functions
    .External.graphics (used in graphics) and  .Call.graphics (used in grid).

    If there is an error or user-interrupt in the above
    evaluation, dd->recordGraphics is set to TRUE
    on all graphics devices (see GEonExit(); called in errors.c)

    NOTE: if someone uses try() around this call and there
    is an error, then dd->recordGraphics stays FALSE, so
    subsequent pages of graphics output are NOT saved on
    the display list.  A workaround is to deliberately
    force an error in a graphics call (e.g., a grid popViewport()
    while in the ROOT viewport) which will reset dd->recordGraphics
    to TRUE as per the comment above.
*/

#include <R_ext/GraphicsEngine.h>

attribute_hidden SEXP do_Externalgr(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP retval;
    pGEDevDesc dd = GEcurrentDevice();
    bool record = dd->recordGraphics;
#ifdef R_GE_DEBUG
    if (getenv("R_GE_DEBUG_record")) {
        printf("do_Externalgr: record = FALSE\n");
    }
#endif
    dd->recordGraphics = false;
    PROTECT(retval = do_External(call, op, args, env));
#ifdef R_GE_DEBUG
    if (getenv("R_GE_DEBUG_record")) {
        printf("do_Externalgr: record = %d\n", record);
    }
#endif
    dd->recordGraphics = record;
    if (GErecording(call, dd)) { // which is record && call != R_NilValue
	if (!GEcheckState(dd))
	    errorcall(call, _("invalid graphics state"));
	/* args is escaping, so make sure it is reference counting. */
	/* should already be handled in do_External, but be safe ... */
	R_args_enable_refcnt(args);
	GErecordGraphicOperation(op, args, dd);
    }
    check_retval(call, retval);
    UNPROTECT(1);
    return retval;
}

attribute_hidden SEXP do_dotcallgr(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP retval;
    pGEDevDesc dd = GEcurrentDevice();
    bool record = dd->recordGraphics;
#ifdef R_GE_DEBUG
    if (getenv("R_GE_DEBUG_record")) {
        printf("do_dotcallgr: record = FALSE\n");
    }
#endif
    dd->recordGraphics = false;
    PROTECT(retval = do_dotcall(call, op, args, env));
#ifdef R_GE_DEBUG
    if (getenv("R_GE_DEBUG_record")) {
        printf("do_dotcallgr: record = %d\n", record);
    }
#endif
    dd->recordGraphics = record;
    if (GErecording(call, dd)) {
	if (!GEcheckState(dd))
	    errorcall(call, _("invalid graphics state"));
	/* args is escaping, so make sure it is reference counting. */
	R_args_enable_refcnt(args);
	GErecordGraphicOperation(op, args, dd);
    }
    check_retval(call, retval);
    UNPROTECT(1);
    return retval;
}

static SEXP
Rf_getCallingDLL(void)
{
    SEXP e, ans;
    RCNTXT *cptr;
    SEXP rho = R_NilValue;
    bool found = false;

    /* First find the environment of the caller.
       Testing shows this is the right caller, despite the .C/.Call ...
     */
    for (cptr = R_GlobalContext;
	 cptr != NULL && cptr->callflag != CTXT_TOPLEVEL;
	 cptr = cptr->nextcontext)
	    if (cptr->callflag & CTXT_FUNCTION) {
		/* PrintValue(cptr->call); */
		rho = cptr->cloenv;
		break;
	    }
    /* Then search up until we hit a namespace or globalenv.
       The idea is that we will not find a namespace unless the caller
       was defined in one. */
    while(rho != R_NilValue) {
	if (rho == R_GlobalEnv) break;
	else if (R_IsNamespaceEnv(rho)) {
	    found = true;
	    break;
	}
	rho = ENCLOS(rho);
    }
    if(!found) return R_NilValue;

    PROTECT(e = lang2(install("getCallingDLLe"), rho));
    ans = eval(e,  R_GlobalEnv);
    UNPROTECT(1);
    return(ans);
}


/*
  We are given the PACKAGE argument in dll.obj
  and we can try to figure out how to resolve this.
  0) dll.obj is NULL.  Then find the environment of the
   calling function and if it is a namespace, get the first registered DLL.

  1) dll.obj is a DLLInfo object
*/
static DL_FUNC
R_FindNativeSymbolFromDLL(char *name, DllReference *dll,
			  R_RegisteredNativeSymbol *symbol,
			  SEXP env)
{
    int numProtects = 0;
    DllInfo *info;
    DL_FUNC fun = NULL;

    if(dll->obj == NULL) {
	/* Rprintf("\nsearching for %s\n", name); */
	if (env != R_NilValue) {
	    SEXP e;
	    PROTECT(e = lang2(install("getCallingDLLe"), env));
	    dll->obj = eval(e, R_GlobalEnv);
	    UNPROTECT(1);
	} else dll->obj = Rf_getCallingDLL();
	PROTECT(dll->obj); numProtects++;
    }

    if(inherits(dll->obj, "DLLInfo")) {
	SEXP tmp;
	tmp = VECTOR_ELT(dll->obj, 4);
	info = (DllInfo *) R_ExternalPtrAddr(tmp);
	if(!info)
	    error(_("NULL value for DLLInfoReference when looking for DLL"));
	if (info->forceSymbols)
	    error(_("DLL requires the use of native symbols"));
	fun = R_dlsym(info, name, symbol);
    }

    if(numProtects) UNPROTECT(numProtects);

    return fun;
}



/* .C() {op=0}  or  .Fortran() {op=1} */
/* Use of this except for atomic vectors is not allowed for .Fortran,
   and is only kept for legacy code for .C.

   CRAN packages R2Cuba, RCALI, ars, coxme, fCopulae, locfit, nlme,
   splinesurv and survival pass functions, the case of RCALI as a list
   of two functions.

   RecordLinkage and locfit pass lists.
*/

/* pattern and number of guard bytes */
#define FILL 0xee
#define NG 64

attribute_hidden SEXP do_dotCode(SEXP call, SEXP op, SEXP args, SEXP env)
{
    int Fort = PRIMVAL(op);

	BEGIN_TIMER_ALTERNATIVES(Fort, TR_dotFortranFull, TR_dotCFull);

	void **cargs, **cargs0 = NULL /* -Wall */;
    int naok, na, nargs;
    bool copy = R_CBoundsCheck; /* options(CboundsCheck) */
    DL_FUNC fun = NULL;
    SEXP ans, pa, s;
    R_RegisteredNativeSymbol symbol = {R_C_SYM, {NULL}, NULL};
    R_NativePrimitiveArgType *checkTypes = NULL;
    const void *vmax;
    char symName[MaxSymbolBytes];

    if (length(args) < 1) errorcall(call, _("'.NAME' is missing"));
    check1arg2(args, call, ".NAME");
    if (NaokSymbol == NULL || DupSymbol == NULL || PkgSymbol == NULL) {
	NaokSymbol = install("NAOK");
	DupSymbol = install("DUP");
	PkgSymbol = install("PACKAGE");
    }
    if (EncSymbol == NULL) EncSymbol = install("ENCODING");
    if (CSingSymbol == NULL) CSingSymbol = install("Csingle");
    vmax = vmaxget();
    Fort = PRIMVAL(op);
    if(Fort) symbol.type = R_FORTRAN_SYM;

    args = enctrim(args);
    args = resolveNativeRoutine(args, &fun, &symbol, symName, &nargs,
				&naok, call, env);

    if(symbol.symbol.c && symbol.symbol.c->numArgs > -1) {
	if(symbol.symbol.c->numArgs != nargs)
	    errorcall(call,
		      _("Incorrect number of arguments (%d), expecting %d for '%s'"),
		      nargs, symbol.symbol.c->numArgs, symName);

	checkTypes = symbol.symbol.c->types;
    }

    /* Construct the return value */
    nargs = 0;
    bool havenames = false;
    for(pa = args ; pa != R_NilValue; pa = CDR(pa)) {
	if (TAG(pa) != R_NilValue) havenames = true;
	nargs++;
    }

    PROTECT(ans = allocVector(VECSXP, nargs));
    if (havenames) {
	SEXP names;
	PROTECT(names = allocVector(STRSXP, nargs));
	for (na = 0, pa = args ; pa != R_NilValue ; pa = CDR(pa), na++) {
	    if (TAG(pa) == R_NilValue)
		SET_STRING_ELT(names, na, R_BlankString);
	    else
		SET_STRING_ELT(names, na, PRINTNAME(TAG(pa)));
	}
	setAttrib(ans, R_NamesSymbol, names);
	UNPROTECT(1);
    }

    /* Convert the arguments for use in foreign function calls. */
    cargs = (void**) R_alloc(nargs, sizeof(void*));
    if (copy) cargs0 = (void**) R_alloc(nargs, sizeof(void*));
    for(na = 0, pa = args ; pa != R_NilValue; pa = CDR(pa), na++) {
	if(checkTypes &&
	   !comparePrimitiveTypes(checkTypes[na], CAR(pa))) {
	    /* We can loop over all the arguments and report all the
	       erroneous ones, but then we would also want to avoid
	       the conversions.  Also, in the future, we may just
	       attempt to coerce the value to the appropriate
	       type. */
	    errorcall(call, _("wrong type for argument %d in call to %s"),
		      na+1, symName);
	}
	int nprotect = 0, targetType =  checkTypes ? checkTypes[na] : 0;
	R_xlen_t n;
	s = CAR(pa);
	/* start with return value a copy of the inputs, as that is
	   what is needed for non-atomic-vector inputs */
	SET_VECTOR_ELT(ans, na, s);

	if(checkNativeType(targetType, TYPEOF(s)) == false &&
	   targetType != SINGLESXP) {
	    /* Cannot be called if DUP = FALSE, so only needs to live
	       until copied in the switch.
	       But R_alloc allocates, so missed protection < R 2.15.0.
	    */
	    PROTECT(s = coerceVector(s, targetType));
	    nprotect++;
	}

	/* We create any copies needed for the return value here,
	   except for character vectors.  The compiled code works on
	   the data pointer of the return value for the other atomic
	   vectors, and anything else is supposed to be read-only.

	   We do not need to copy if the inputs have no references */

#ifdef LONG_VECTOR_SUPPORT
	if (isVector(s) && IS_LONG_VEC(s))
	    error(_("long vectors (argument %d) are not supported in %s"),
		  na + 1, Fort ? ".Fortran" : ".C");
#endif
	SEXPTYPE t = TYPEOF(s);
	switch(t) {
	case RAWSXP:
	    if (copy) {
		n = XLENGTH(s);
		char *ptr = R_alloc(n * sizeof(Rbyte) + 2 * NG, 1);
		memset(ptr, FILL, n * sizeof(Rbyte) + 2 * NG);
		ptr += NG;
		if (n) memcpy(ptr, RAW(s), n);
		cargs[na] = (void *) ptr;
	    } else if (MAYBE_REFERENCED(s)) {
		n = XLENGTH(s);
		SEXP ss = allocVector(t, n);
		if (n) memcpy(RAW(ss), RAW(s), n * sizeof(Rbyte));
		SET_VECTOR_ELT(ans, na, ss);
		cargs[na] = (void*) RAW(ss);
#ifdef R_MEMORY_PROFILING
		if (RTRACE(s)) memtrace_report(s, ss);
#endif
	    } else cargs[na] = (void *) RAW(s);
	    break;
	case LGLSXP:
	case INTSXP:
	    n = XLENGTH(s);
	    int *iptr = INTEGER(s);
	    if (!naok)
		for (R_xlen_t i = 0 ; i < n ; i++)
		    if(iptr[i] == NA_INTEGER)
			error(_("NAs in foreign function call (arg %d)"), na + 1);
	    if (copy) {
		char *ptr = R_alloc(n * sizeof(int) + 2 * NG, 1);
		memset(ptr, FILL, n * sizeof(int) + 2 * NG);
		ptr += NG;
		if (n) memcpy(ptr, INTEGER(s), n * sizeof(int));
		cargs[na] = (void*) ptr;
	    } else if (MAYBE_REFERENCED(s)) {
		SEXP ss = allocVector(t, n);
		if (n) memcpy(INTEGER(ss), INTEGER(s), n * sizeof(int));
		SET_VECTOR_ELT(ans, na, ss);
		cargs[na] = (void*) INTEGER(ss);
#ifdef R_MEMORY_PROFILING
		if (RTRACE(s)) memtrace_report(s, ss);
#endif
	    } else cargs[na] = (void*) iptr;
	    break;
	case REALSXP:
	    n = XLENGTH(s);
	    double *rptr = REAL(s);
	    if (!naok)
		for (R_xlen_t i = 0 ; i < n ; i++)
		    if(!R_FINITE(rptr[i]))
			error(_("NA/NaN/Inf in foreign function call (arg %d)"), na + 1);
	    if (asLogical(getAttrib(s, CSingSymbol)) == 1) {
		float *sptr = (float*) R_alloc(n, sizeof(float));
		for (R_xlen_t i = 0 ; i < n ; i++) sptr[i] = (float) REAL(s)[i];
		cargs[na] = (void*) sptr;
#ifdef R_MEMORY_PROFILING
		if (RTRACE(s)) memtrace_report(s, sptr);
#endif
	    } else if (copy) {
		char *ptr = R_alloc(n * sizeof(double) + 2 * NG, 1);
		memset(ptr, FILL, n * sizeof(double) + 2 * NG);
		ptr += NG;
		if (n) memcpy(ptr, REAL(s), n * sizeof(double));
		cargs[na] = (void*) ptr;
	    } else if (MAYBE_REFERENCED(s)) {
		SEXP ss  = allocVector(t, n);
		if (n) memcpy(REAL(ss), REAL(s), n * sizeof(double));
		SET_VECTOR_ELT(ans, na, ss);
		cargs[na] = (void*) REAL(ss);
#ifdef R_MEMORY_PROFILING
		if (RTRACE(s)) memtrace_report(s, ss);
#endif
	    } else cargs[na] = (void*) rptr;
	    break;
	case CPLXSXP:
	    n = XLENGTH(s);
	    Rcomplex *zptr = COMPLEX(s);
	    if (!naok)
		for (R_xlen_t i = 0 ; i < n ; i++)
		    if(!R_FINITE(zptr[i].r) || !R_FINITE(zptr[i].i))
			error(_("complex NA/NaN/Inf in foreign function call (arg %d)"), na + 1);
	    if (copy) {
		char *ptr = R_alloc(n * sizeof(Rcomplex) + 2 * NG, 1);
		memset(ptr, FILL, n * sizeof(Rcomplex) + 2 * NG);
		ptr += NG;
		if (n) memcpy(ptr, COMPLEX(s), n * sizeof(Rcomplex));
		cargs[na] = (void*) ptr;
	    } else if (MAYBE_REFERENCED(s)) {
		SEXP ss = allocVector(t, n);
		if (n) memcpy(COMPLEX(ss), COMPLEX(s), n * sizeof(Rcomplex));
		SET_VECTOR_ELT(ans, na, ss);
		cargs[na] = (void*) COMPLEX(ss);
#ifdef R_MEMORY_PROFILING
		if (RTRACE(s)) memtrace_report(s, ss);
#endif
	    } else cargs[na] = (void *) zptr;
	    break;
	case STRSXP:
	    n = XLENGTH(s);
	    if (Fort) {
		const char *ss = translateChar(STRING_ELT(s, 0));
		if (n > 1)
		    warning("only the first string in a char vector used in .Fortran");
		else
		    warning("passing a char vector to .Fortran is not portable");
		char *fptr = (char*) R_alloc(max(255, strlen(ss)) + 1, sizeof(char));
		strcpy(fptr, ss);
		cargs[na] =  (void*) fptr;
	    } else if (copy) {
		char **cptr = (char**) R_alloc(n, sizeof(char*)),
		    **cptr0 = (char**) R_alloc(n, sizeof(char*));
		for (R_xlen_t i = 0 ; i < n ; i++) {
		    const char *ss = translateChar(STRING_ELT(s, i));
		    size_t nn = strlen(ss) + 1 + 2 * NG;
		    char *ptr = (char*) R_alloc(nn, sizeof(char));
		    memset(ptr, FILL, nn);
		    cptr[i] = cptr0[i] = ptr + NG;
		    strcpy(cptr[i], ss);
		}
		cargs[na] = (void*) cptr;
		cargs0[na] = (void*) cptr0;
#ifdef R_MEMORY_PROFILING
		if (RTRACE(s)) memtrace_report(s, cargs[na]);
#endif
	    } else {
		char **cptr = (char**) R_alloc(n, sizeof(char*));
		for (R_xlen_t i = 0 ; i < n ; i++) {
		    const char *ss = translateChar(STRING_ELT(s, i));
		    size_t nn = strlen(ss) + 1;
		    if(nn > 1) {
			cptr[i] = (char*) R_alloc(nn, sizeof(char));
			strcpy(cptr[i], ss);
		    } else {
			/* Protect ourselves against those who like to
			   extend "", maybe using strncpy */
			nn = 128;
			cptr[i] = (char*) R_alloc(nn, sizeof(char));
			memset(cptr[i], 0, nn);
		    }
		}
		cargs[na] = (void*) cptr;
#ifdef R_MEMORY_PROFILING
		if (RTRACE(s)) memtrace_report(s, cargs[na]);
#endif
	    }
	    break;
	case VECSXP:
	    if (Fort) error(_("invalid mode (%s) to pass to Fortran (arg %d)"),
			    R_typeToChar(s), na + 1);
	    /* Used read-only, so this is safe */
#ifdef USE_RINTERNALS
            if (!ALTREP(s))
                cargs[na] = (void*) DATAPTR(s);
            else {
#else
                n = XLENGTH(s);
                SEXP *lptr = (SEXP *) R_alloc(n, sizeof(SEXP));
                for (R_xlen_t i = 0 ; i < n ; i++) lptr[i] = VECTOR_ELT(s, i);
                cargs[na] = (void*) lptr;
#endif
#ifdef USE_RINTERNALS
            }
#endif
            break;
	case CLOSXP:
	case BUILTINSXP:
	case SPECIALSXP:
	case ENVSXP:
	    if (Fort) error(_("invalid mode (%s) to pass to Fortran (arg %d)"),
			    R_typeToChar(s), na + 1);
	    cargs[na] =  (void*) s;
	    break;
	case NILSXP:
	    error(_("invalid mode (%s) to pass to C or Fortran (arg %d)"),
		  R_typeToChar(s), na + 1);
	    cargs[na] =  (void*) s;
	    break;
	default:
	    /* Includes pairlists from R 2.15.0 */
	    if (Fort) error(_("invalid mode (%s) to pass to Fortran (arg %d)"),
			    R_typeToChar(s), na + 1);
	    warning("passing an object of type '%s' to .C (arg %d) is deprecated",
		    R_typeToChar(s), na + 1);
	    if (t == LISTSXP)
		warning(_("pairlists are passed as SEXP as from R 2.15.0"));
	    cargs[na] =  (void*) s;
	    break;
	}
	if (nprotect) UNPROTECT(nprotect);
    }

	BEGIN_TIMER_ALTERNATIVES(Fort, TR_dotFortran, TR_dotC);
    BEGIN_EXTERNAL_TIMER(symName, ofun);

    /* FIXME: Calling a function via an incompatible function pointer is
       undefined behavior. */ 
    switch (nargs) {
    case 0:
	((FUNV0)fun)();
	break;
    case 1:
	((FUNV1)fun)(cargs[0]);
	break;
    case 2:
	((FUNV2)fun)(cargs[0], cargs[1]);
	break;
    case 3:
	((FUNV3)fun)(cargs[0], cargs[1], cargs[2]);
	break;
    case 4:
	((FUNV4)fun)(cargs[0], cargs[1], cargs[2], cargs[3]);
	break;
    case 5:
	((FUNV5)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4]);
	break;
    case 6:
	((FUNV6)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5]);
	break;
    case 7:
	((FUNV7)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6]);
	break;
    case 8:
	((FUNV8)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7]);
	break;
    case 9:
	((FUNV9)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8]);
	break;
    case 10:
	((FUNV10)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9]);
	break;
    case 11:
	((FUNV11)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10]);
	break;
    case 12:
	((FUNV12)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11]);
	break;
    case 13:
	((FUNV13)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12]);
	break;
    case 14:
	((FUNV14)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13]);
	break;
    case 15:
	((FUNV15)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14]);
	break;
    case 16:
	((FUNV16)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15]);
	break;
    case 17:
	((FUNV17)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16]);
	break;
    case 18:
	((FUNV18)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17]);
	break;
    case 19:
	((FUNV19)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18]);
	break;
    case 20:
	((FUNV20)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19]);
	break;
    case 21:
	((FUNV21)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20]);
	break;
    case 22:
	((FUNV22)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21]);
	break;
    case 23:
	((FUNV23)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22]);
	break;
    case 24:
	((FUNV24)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23]);
	break;
    case 25:
	((FUNV25)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24]);
	break;
    case 26:
	((FUNV26)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25]);
	break;
    case 27:
	((FUNV27)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26]);
	break;
    case 28:
	((FUNV28)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27]);
	break;
    case 29:
	((FUNV29)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28]);
	break;
    case 30:
	((FUNV30)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29]);
	break;
    case 31:
	((FUNV31)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30]);
	break;
    case 32:
	((FUNV32)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31]);
	break;
    case 33:
	((FUNV33)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32]);
	break;
    case 34:
	((FUNV34)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33]);
	break;
    case 35:
	((FUNV35)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34]);
	break;
    case 36:
	((FUNV36)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35]);
	break;
    case 37:
	((FUNV37)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36]);
	break;
    case 38:
	((FUNV38)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37]);
	break;
    case 39:
	((FUNV39)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38]);
	break;
    case 40:
	((FUNV40)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39]);
	break;
    case 41:
	((FUNV41)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40]);
	break;
    case 42:
	((FUNV42)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41]);
	break;
    case 43:
	((FUNV43)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42]);
	break;
    case 44:
	((FUNV44)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43]);
	break;
    case 45:
	((FUNV45)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44]);
	break;
    case 46:
	((FUNV46)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45]);
	break;
    case 47:
	((FUNV47)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46]);
	break;
    case 48:
	((FUNV48)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47]);
	break;
    case 49:
	((FUNV49)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48]);
	break;
    case 50:
	((FUNV50)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49]);
	break;
    case 51:
	((FUNV51)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50]);
	break;
    case 52:
	((FUNV52)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51]);
	break;
    case 53:
	((FUNV53)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52]);
	break;
    case 54:
	((FUNV54)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53]);
	break;
    case 55:
	((FUNV55)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54]);
	break;
    case 56:
	((FUNV56)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55]);
	break;
    case 57:
	((FUNV57)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56]);
	break;
    case 58:
	((FUNV58)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57]);
	break;
    case 59:
	((FUNV59)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58]);
	break;
    case 60:
	((FUNV60)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58], cargs[59]);
	break;
    case 61:
	((FUNV61)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58], cargs[59],
	    cargs[60]);
	break;
    case 62:
	((FUNV62)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58], cargs[59],
	    cargs[60], cargs[61]);
	break;
    case 63:
	((FUNV63)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58], cargs[59],
	    cargs[60], cargs[61], cargs[62]);
	break;
    case 64:
	((FUNV64)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58], cargs[59],
	    cargs[60], cargs[61], cargs[62], cargs[63]);
	break;
    case 65:
	((FUNV65)fun)(cargs[0],  cargs[1],  cargs[2],  cargs[3],  cargs[4],
	    cargs[5],  cargs[6],  cargs[7],  cargs[8],  cargs[9],
	    cargs[10], cargs[11], cargs[12], cargs[13], cargs[14],
	    cargs[15], cargs[16], cargs[17], cargs[18], cargs[19],
	    cargs[20], cargs[21], cargs[22], cargs[23], cargs[24],
	    cargs[25], cargs[26], cargs[27], cargs[28], cargs[29],
	    cargs[30], cargs[31], cargs[32], cargs[33], cargs[34],
	    cargs[35], cargs[36], cargs[37], cargs[38], cargs[39],
	    cargs[40], cargs[41], cargs[42], cargs[43], cargs[44],
	    cargs[45], cargs[46], cargs[47], cargs[48], cargs[49],
	    cargs[50], cargs[51], cargs[52], cargs[53], cargs[54],
	    cargs[55], cargs[56], cargs[57], cargs[58], cargs[59],
	    cargs[60], cargs[61], cargs[62], cargs[63], cargs[64]);
	break;
    default:
	errorcall(call, _("too many arguments, sorry"));
    }

	END_EXTERNAL_TIMER();
    END_TIMER_ALTERNATIVES(Fort, TR_dotFortran, TR_dotC);
    for (na = 0, pa = args ; pa != R_NilValue ; pa = CDR(pa), na++) {
	void *p = cargs[na];
	SEXP arg = CAR(pa);
	s = VECTOR_ELT(ans, na);
	R_NativePrimitiveArgType type =
	    checkTypes ? checkTypes[na] : TYPEOF(arg);
	R_xlen_t n = xlength(arg);

	switch(type) {
	case RAWSXP:
	    if (copy) {
		s = allocVector(type, n);
		unsigned char *ptr = (unsigned char *) p;
		if (n) memcpy(RAW(s), ptr, n * sizeof(Rbyte));
		ptr += n * sizeof(Rbyte);
		for (int i = 0; i < NG; i++)
		    if(*ptr++ != FILL)
			error("array over-run in %s(\"%s\") in %s argument %d\n",
			      Fort ? ".Fortran" : ".C",
			      symName, type2char(type), na+1);
		ptr = (unsigned char *) p;
		for (int i = 0; i < NG; i++)
		    if(*--ptr != FILL)
			error("array under-run in %s(\"%s\") in %s argument %d\n",
			      Fort ? ".Fortran" : ".C",
			      symName, type2char(type), na+1);
	    }
	    break;
	case INTSXP:
	    if (copy) {
		s = allocVector(type, n);
		unsigned char *ptr = (unsigned char *) p;
		if (n) memcpy(INTEGER(s), ptr, n * sizeof(int));
		ptr += n * sizeof(int);
		for (int i = 0; i < NG; i++)
		    if(*ptr++ != FILL)
			error("array over-run in %s(\"%s\") in %s argument %d\n",
			      Fort ? ".Fortran" : ".C",
			      symName, type2char(type), na+1);
		ptr = (unsigned char *) p;
		for (int i = 0; i < NG; i++)
		    if(*--ptr != FILL)
			error("array under-run in %s(\"%s\") in %s argument %d\n",
			      Fort ? ".Fortran" : ".C",
			      symName, type2char(type), na+1);
	    }
	    break;
	case LGLSXP:
	    if (copy) {
		s = allocVector(type, n);
		unsigned char *ptr = (unsigned char *) p;
		int *iptr = (int*) ptr, tmp;
		for (R_xlen_t i = 0 ; i < n ; i++) {
		    tmp =  iptr[i];
		    LOGICAL(s)[i] = (tmp == NA_INTEGER || tmp == 0) ? tmp : 1;
		}
		ptr += n * sizeof(int);
		for (int i = 0; i < NG;  i++)
		    if(*ptr++ != FILL)
			error("array over-run in %s(\"%s\") in %s argument %d\n",
			      Fort ? ".Fortran" : ".C",
			      symName, type2char(type), na+1);
		ptr = (unsigned char *) p;
		for (int i = 0; i < NG; i++)
		    if(*--ptr != FILL)
			error("array under-run in %s(\"%s\") in %s argument %d\n",
			      Fort ? ".Fortran" : ".C",
			      symName, type2char(type), na+1);
	    } else {
		int *iptr = (int *)p, tmp;
		for (R_xlen_t i = 0 ; i < n ; i++) {
		    tmp =  iptr[i];
		    iptr[i] = (tmp == NA_INTEGER || tmp == 0) ? tmp : 1;
		}
	    }
	    break;
	case REALSXP:
	case SINGLESXP:
	    if (copy) {
		PROTECT(s = allocVector(REALSXP, n));
		if (type == SINGLESXP || asLogical(getAttrib(arg, CSingSymbol)) == 1) {
		    float *sptr = (float*) p;
		    for(R_xlen_t i = 0 ; i < n ; i++)
			REAL(s)[i] = (double) sptr[i];
		} else {
		    unsigned char *ptr = (unsigned char *) p;
		    if (n) memcpy(REAL(s), ptr, n * sizeof(double));
		    ptr += n * sizeof(double);
		    for (int i = 0; i < NG; i++)
			if(*ptr++ != FILL)
			    error("array over-run in %s(\"%s\") in %s argument %d\n",
				  Fort ? ".Fortran" : ".C",
				  symName, type2char(type), na+1);
		    ptr = (unsigned char *) p;
		    for (int i = 0; i < NG; i++)
			if(*--ptr != FILL)
			    error("array under-run in %s(\"%s\") in %s argument %d\n",
				  Fort ? ".Fortran" : ".C",
				  symName, type2char(type), na+1);
		}
		UNPROTECT(1); /* s */
	    } else {
		if (type == SINGLESXP || asLogical(getAttrib(arg, CSingSymbol)) == 1) {
		    s = allocVector(REALSXP, n);
		    float *sptr = (float*) p;
		    for(int i = 0 ; i < n ; i++)
			REAL(s)[i] = (double) sptr[i];
		}
	    }
	    break;
	case CPLXSXP:
	    if (copy) {
		s = allocVector(type, n);
		unsigned char *ptr = (unsigned char *) p;
		if (n) memcpy(COMPLEX(s), p, n * sizeof(Rcomplex));
		ptr += n * sizeof(Rcomplex);
		for (int i = 0; i < NG;  i++)
		    if(*ptr++ != FILL)
			error("array over-run in %s(\"%s\") in %s argument %d\n",
			      Fort ? ".Fortran" : ".C",
			      symName, type2char(type), na+1);
		ptr = (unsigned char *) p;
		for (int i = 0; i < NG; i++)
		    if(*--ptr != FILL)
			error("array under-run in %s(\"%s\") in %s argument %d\n",
			      Fort ? ".Fortran" : ".C",
			      symName, type2char(type), na+1);
	    }
	    break;
	case STRSXP:
	    if(Fort) {
		char buf[256];
		/* only return one string: warned on the R -> Fortran step */
		strncpy(buf, (char*)p, 255);
		buf[255] = '\0';
		PROTECT(s = allocVector(type, 1));
		SET_STRING_ELT(s, 0, mkChar(buf));
		UNPROTECT(1);
	    } else if (copy) {
		SEXP ss = arg;
		PROTECT(s = allocVector(type, n));
		char **cptr = (char**) p, **cptr0 = (char**) cargs0[na];
		for (R_xlen_t i = 0 ; i < n ; i++) {
		    unsigned char *ptr = (unsigned char *) cptr[i];
		    SET_STRING_ELT(s, i, mkChar(cptr[i]));
		    if (cptr[i] == cptr0[i]) {
			const char *z = translateChar(STRING_ELT(ss, i));
			for (int j = 0; j < NG; j++)
			    if(*--ptr != FILL)
				error("array under-run in .C(\"%s\") in character argument %d, element %d",
				      symName, na+1, (int)(i+1));
			ptr = (unsigned char *) cptr[i];
			ptr += strlen(z) + 1;
			for (int j = 0; j < NG;  j++)
			    if(*ptr++ != FILL) {
				// force termination
				unsigned char *p = ptr;
				for (int k = 1; k < NG - j; k++, p++)
				    if (*p == FILL) *p = '\0';
				error("array over-run in .C(\"%s\") in character argument %d, element %d\n'%s'->'%s'\n",
				      symName, na+1, (int)(i+1),
				      z, cptr[i]);
			    }
		    }
		}
		UNPROTECT(1);
	    } else {
		PROTECT(s = allocVector(type, n));
		char **cptr = (char**) p;
		for (R_xlen_t i = 0 ; i < n ; i++)
		    SET_STRING_ELT(s, i, mkChar(cptr[i]));
		UNPROTECT(1);
	    }
	    break;
	default:
	    break;
	}
	if (s != arg) {
	    PROTECT(s);
	    SHALLOW_DUPLICATE_ATTRIB(s, arg);
	    SET_VECTOR_ELT(ans, na, s);
	    UNPROTECT(1);
	}
    }
    UNPROTECT(1);
    vmaxset(vmax);

	END_TIMER_ALTERNATIVES(Fort, TR_dotFortranFull, TR_dotCFull);

    return ans;
}
