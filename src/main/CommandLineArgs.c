/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1997-2023   The R Core Team
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
#include <config.h>
#endif

#include <string.h>

#include <Defn.h>
#include <R_ext/RStartup.h>

#include "timeR.h"

/* Remove and process common command-line arguments
 *  Formally part of ../unix/sys-common.c.
 */

/*
  This copies the command line arguments to the Rstart
  structure. The memory is obtained from calloc, etc.
  since these are permanent and it is not intended that
  they be modified. This is why they are copied before
  being processed and removed from the list.

  We might store these as a SEXP. I have no strong opinion
  about this.
 */

/* Permanent copy of the command line arguments and the number
   of them passed to the application.
   These are populated via the routine R_set_command_line_arguments().
*/
static int    NumCommandLineArgs = 0;
static char **CommandLineArgs = NULL; // this does not get freed


void
R_set_command_line_arguments(int argc, char **argv)
{
    // nothing here is ever freed.
    NumCommandLineArgs = argc;
    CommandLineArgs = (char**) calloc((size_t) argc, sizeof(char*));
    if(CommandLineArgs == NULL)
	R_Suicide("allocation failure in R_set_command_line_arguments");

    for(int i = 0; i < argc; i++) {
	CommandLineArgs[i] = strdup(argv[i]);
	if(CommandLineArgs[i] == NULL)
	    R_Suicide("allocation failure in R_set_command_line_arguments");
    }
}


/*
  The .Internal which returns the command line arguments that are stored
  in global variables.
 */
attribute_hidden
SEXP do_commandArgs(SEXP call, SEXP op, SEXP args, SEXP env)
{
    int i;
    SEXP vals;

    checkArity(op, args);
    /* need protection as mkChar allocates */
    vals = PROTECT(allocVector(STRSXP, NumCommandLineArgs));
    for(i = 0; i < NumCommandLineArgs; i++)
	SET_STRING_ELT(vals, i, mkChar(CommandLineArgs[i]));
    UNPROTECT(1);
    return vals;
}

#ifdef Win32
extern Rboolean R_LoadRconsole;
#endif

void
R_common_command_line(int *pac, char **argv, Rstart Rp)
{
    int ac = *pac, newac = 1;	/* argv[0] is process name */
    long lval; /* this is used for ppval, so 32-bit long is fine */
    char *p, **av = argv, msg[1024];
    Rboolean processing = TRUE;

    R_RestoreHistory = 1;
    while(--ac) {
	if(processing && **++av == '-') {
	    if (!strcmp(*av, "--version")) {
		PrintVersion(msg, 1024);
		R_ShowMessage(msg);
		exit(0);
	    }
	    else if(!strcmp(*av, "--args")) {
		/* copy this through for further processing */
		argv[newac++] = *av;
		processing = FALSE;
	    }
	    else if(!strcmp(*av, "--save")) {
		Rp->SaveAction = SA_SAVE;
	    }
	    else if(!strcmp(*av, "--no-save")) {
		Rp->SaveAction = SA_NOSAVE;
	    }
	    else if(!strcmp(*av, "--restore")) {
		Rp->RestoreAction = SA_RESTORE;
	    }
	    else if(!strcmp(*av, "--no-restore")) {
		Rp->RestoreAction = SA_NORESTORE;
		R_RestoreHistory = 0;
	    }
	    else if(!strcmp(*av, "--no-restore-data")) {
		Rp->RestoreAction = SA_NORESTORE;
	    }
	    else if(!strcmp(*av, "--no-restore-history")) {
		R_RestoreHistory = 0;
	    }
	    else if (!strcmp(*av, "--silent") ||
		     !strcmp(*av, "--quiet") ||
		     !strcmp(*av, "-q")) {
		Rp->R_Quiet = TRUE;
	    }
	    else if (!strcmp(*av, "--vanilla")) {
		Rp->SaveAction = SA_NOSAVE; /* --no-save */
		Rp->RestoreAction = SA_NORESTORE; /* --no-restore */
		R_RestoreHistory = 0;     // --no-restore-history (= part of --no-restore)
		Rp->LoadSiteFile = FALSE; /* --no-site-file */
		Rp->LoadInitFile = FALSE; /* --no-init-file */
		Rp->NoRenviron = TRUE;    // --no-environ
#ifdef Win32
		R_LoadRconsole = FALSE;
#endif
	    }
	    else if (!strcmp(*av, "--no-environ")) {
		Rp->NoRenviron = TRUE;
	    }
	    else if (!strcmp(*av, "--verbose")) {
		Rp->R_Verbose = TRUE;
	    }
	    else if (!strcmp(*av, "--no-echo") ||
		     !strcmp(*av, "--slave") || // "deprecated" from R 4.0.0 (spring 2020)
		     !strcmp(*av, "-s")) {
		Rp->R_Quiet = TRUE;
		Rp->R_NoEcho = TRUE;
		Rp->SaveAction = SA_NOSAVE;
	    }
	    else if (!strcmp(*av, "--no-site-file")) {
		Rp->LoadSiteFile = FALSE;
	    }
	    else if (!strcmp(*av, "--no-init-file")) {
		Rp->LoadInitFile = FALSE;
	    }
	    /* Undocumented and unused.
	     else if (!strcmp(*av, "--debug-init")) {
	    	Rp->DebugInitFile = TRUE;
	     */
	    else if (!strncmp(*av, "--encoding", 10)) {
		if(strlen(*av) < 12) {
		    if(ac > 1) {ac--; av++; p = *av;} else p = NULL;
		} else p = &(*av)[11];
		if (p == NULL) {
		    R_ShowMessage(_("WARNING: no value given for --encoding"));
		} else {
		    strncpy(R_StdinEnc, p, 30);
		    R_StdinEnc[30] = '\0';
		}
	    }
#ifdef Win32
	    else if (!strcmp(*av, "--no-Rconsole")) {
		R_LoadRconsole = 0;
	    }
#endif
	    else if (!strcmp(*av, "-save") ||
		     !strcmp(*av, "-nosave") ||
		     !strcmp(*av, "-restore") ||
		     !strcmp(*av, "-norestore") ||
		     !strcmp(*av, "-noreadline") ||
		     !strcmp(*av, "-quiet") ||
		     !strcmp(*av, "-nsize") ||
		     !strcmp(*av, "-vsize") ||
		     !strncmp(*av, "--max-nsize", 11) ||
		     !strncmp(*av, "--max-vsize", 11) ||
		     !strcmp(*av, "-V") ||
		     !strcmp(*av, "-n") ||
		     !strcmp(*av, "-v")) {
		snprintf(msg, 1024,
			 _("WARNING: option '%s' no longer supported"), *av);
		R_ShowMessage(msg);
	    }
	    /* mop up --min-[nv]size */
	    else if( !strncmp(*av, "--min-nsize", 11) ||
		     !strncmp(*av, "--min-vsize", 11) ) {
		if(strlen(*av) < 13) {
		    if(ac > 1) {ac--; av++; p = *av;} else p = NULL;
		} else p = &(*av)[12];
		if (p == NULL) {
		    snprintf(msg, 1024,
			     _("WARNING: no value given for '%s'"), *av);
		    R_ShowMessage(msg);
		    break;
		}
		int ierr;
		R_size_t value;
		value = R_Decode2Long(p, &ierr);
		if(ierr) {
		    if(ierr < 0)
			snprintf(msg, 1024,
				 _("WARNING: '%s' value is invalid: ignored"),
				 *av);
		    else
			snprintf(msg, 1024,
				 _("WARNING: %s: too large and ignored"),
				 *av);
		    R_ShowMessage(msg);

		} else {
		    if(!strncmp(*av, "--min-nsize", 11)) Rp->nsize = value;
		    if(!strncmp(*av, "--min-vsize", 11)) Rp->vsize = value;
		}
	    }
	    else if(strncmp(*av, "--max-ppsize", 12) == 0) {
		if(strlen(*av) < 14) {
		    if(ac > 1) {ac--; av++; p = *av;} else p = NULL;
		} else p = &(*av)[13];
		if (p == NULL) {
		    R_ShowMessage(_("WARNING: no value given for '--max-ppsize'"));
		    break;
		}
		lval = strtol(p, &p, 10);
		if (lval < 0)
		    R_ShowMessage(_("WARNING: '--max-ppsize' value is negative: ignored"));
		else if (lval < 10000)
		    R_ShowMessage(_("WARNING: '--max-ppsize' value is too small: ignored"));
		else if (lval > 500000)
		    R_ShowMessage(_("WARNING: '--max-ppsize' value is too large: ignored"));
		else Rp->ppsize = (size_t) lval;
	    }
	    else if(strncmp(*av, "--max-connections", 17) == 0) {
		if(strlen(*av) < 19) {
		    if(ac > 1) {ac--; av++; p = *av;} else p = NULL;
		} else p = &(*av)[18];
		if (p == NULL) {
		    R_ShowMessage(_("WARNING: no value given for '--max-connections'"));
		    break;
		}
		lval = strtol(p, &p, 10);
		if (lval < 0)
		    R_ShowMessage(_("WARNING: '--max-connections' value is negative: ignored"));
		else if (lval < 128)
		    R_ShowMessage(_("WARNING: '--max-connections' value is too small: ignored"));
		else if (lval > 4096)
		    R_ShowMessage(_("WARNING: '--max-connections' value is too large: ignored"));
		else Rp->nconnections = (int) lval;
	    }
#ifdef HAVE_TIME_R
	    else if(strncmp(*av, "--timeR-quiet", 13) == 0) {
		timeR_reduced_output = 1;
	    }

	    else if(strncmp(*av, "--timeR-verbose", 15) == 0) {
		timeR_reduced_output = 0;
	    }

	    else if (strncmp(*av, "--timeR-exclude-init", 20) == 0) {
		timeR_exclude_init = 1;
	    }

	    else if(strncmp(*av, "--timeR-scale", 13) == 0) {
		p = strchr(*av, '=');
		if (p == NULL) {
		    if(ac > 1) {ac--; av++; p = *av;} else p = NULL;
		} else p++;
		if (p == NULL || *p == 0) {
		    snprintf(msg, 1024,
		             _("WARNING: no value given for '%s'"), *av);
		    R_ShowMessage(msg);
		    break;
		}

		lval = strtol(p, &p, 10);
		if (lval < 0)
		    R_ShowMessage(_("WARNING: '--timeR-scale' value is negative: ignored"));
		else if (lval < 1)
		    R_ShowMessage(_("WARNING: '--timeR-scale' value is too small: ignored"));
		else timeR_scale = lval;
	    }

	    else if(strncmp(*av, "--timeR-file", 12) == 0 ||
		    strncmp(*av, "--timeR-raw",  11) == 0 ||
		    (strncmp(*av, "--time", 6) == 0 && (*av)[6] != 'R')) {
		p = strchr(*av, '=');
		if (p == NULL) {
		    if(ac > 1) {ac--; av++; p = *av;} else p = NULL;
		} else p++;
		if (p == NULL || *p == 0) {
		    snprintf(msg, 1024,
		             _("WARNING: no value given for '%s'"), *av);
		    R_ShowMessage(msg);
		    break;
		}
		if(timeR_output_file != NULL) {
		    R_ShowMessage(_("WARNING: multiple timeR output files specified, using last"));
		    free(timeR_output_file);
		}
		timeR_output_file = strdup(p);

		if (strncmp(*av, "--timeR-raw", 11) == 0)
		    timeR_output_raw = 1;
		else
		    timeR_output_raw = 0;

	    }
#endif
	    else { /* unknown -option */
		argv[newac++] = *av;
	    }
	}
	else {
	    argv[newac++] = *av;
	}
    }
    *pac = newac;
    return;
}
