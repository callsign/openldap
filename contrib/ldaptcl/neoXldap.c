/*
 * NeoSoft Tcl client extensions to Lightweight Directory Access Protocol.
 * 
 * Copyright (c) 1998-1999 NeoSoft, Inc.  
 * All Rights Reserved.
 * 
 * This software may be used, modified, copied, distributed, and sold,
 * in both source and binary form provided that these copyrights are
 * retained and their terms are followed.
 * 
 * Under no circumstances are the authors or NeoSoft Inc. responsible
 * for the proper functioning of this software, nor do the authors
 * assume any liability for damages incurred with its use.
 * 
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to NeoSoft, Inc.
 * 
 * NeoSoft, Inc. may not be used to endorse or promote products derived
 * from this software without specific prior written permission. This
 * software is provided ``as is'' without express or implied warranty.
 * 
 * Requests for permission may be sent to NeoSoft Inc, 1770 St. James Place,
 * Suite 500, Houston, TX, 77056.
 *
 * $Id: neoXldap.c,v 1.5 1999/08/03 05:23:03 kunkee Exp $
 *
 */

/*
 * This code was originally developed by Karl Lehenbauer to work with
 * Umich-3.3 LDAP.  It was debugged against the Netscape LDAP server
 * and their much more reliable SDK, and again backported to the
 * Umich-3.3 client code.  The UMICH_LDAP define is used to include
 * code that will work with the Umich-3.3 LDAP, but not with Netscape's
 * SDK.  OpenLDAP may support some of these, but they have not been tested.
 * Currently supported by Randy Kunkee (kunkee@OpenLDAP.org).
 */

/*
 * Add timeout to controlArray to set timeout for ldap_result.
 * 4/14/99 - Randy
 */

#include "tclExtend.h"

#include <lber.h>
#include <ldap.h>
#include <string.h>
#include <sys/time.h>
#include <math.h>

/*
 * Macros to do string compares.  They pre-check the first character before
 * checking of the strings are equal.
 */

#define STREQU(str1, str2) \
	(((str1) [0] == (str2) [0]) && (strcmp (str1, str2) == 0))
#define STRNEQU(str1, str2, n) \
	(((str1) [0] == (str2) [0]) && (strncmp (str1, str2, n) == 0))

/*
 * The following section defines some common macros used by the rest
 * of the code.  It's ugly, and can use some work.  This code was
 * originally developed to work with Umich-3.3 LDAP.  It was debugged
 * against the Netscape LDAP server and the much more reliable SDK,
 * and then again backported to the Umich-3.3 client code.
 */
#define OPEN_LDAP 1
#if defined(OPEN_LDAP)
       /* LDAP_API_VERSION must be defined per the current draft spec
       ** it's value will be assigned RFC number.  However, as
       ** no RFC is defined, it's value is currently implementation
       ** specific (though I would hope it's value is greater than 1823).
       ** In OpenLDAP 2.x-devel, its 2000 + the draft number, ie 2002.
       ** This section is for OPENLDAP.
       */
#define ldap_memfree(p) free(p)
#ifdef LDAP_OPT_ERROR_NUMBER
#define ldap_get_lderrno(ld)	(ldap_get_option(ld, LDAP_OPT_ERROR_NUMBER, &lderrno), lderrno)
#else
#define ldap_get_lderrno(ld) (ld->ld_errno)
#endif
#define LDAP_ERR_STRING(ld)  \
	ldap_err2string(ldap_get_lderrno(ld))
#elif defined( LDAP_OPT_SIZELIMIT )
       /*
       ** Netscape SDK w/ ldap_set_option, ldap_get_option
       */
#define LDAP_ERR_STRING(ld)  \
	ldap_err2string(ldap_get_lderrno(ldap))
#else
       /* U-Mich/OpenLDAP 1.x API */
       /* RFC-1823 w/ changes */
#define UMICH_LDAP 1
#define ldap_memfree(p) free(p)
#define ldap_ber_free(p, n) ber_free(p, n)
#define ldap_value_free_len(bvals) ber_bvecfree(bvals)
#define ldap_get_lderrno(ld) (ld->ld_errno)
#define LDAP_ERR_STRING(ld)  \
	ldap_err2string(ld->ld_errno)
#endif

typedef struct ldaptclobj {
    LDAP	*ldap;
    int		caching;	/* flag 1/0 if caching is enabled */
    long	timeout;	/* timeout from last cache enable */
    long	maxmem;		/* maxmem from last cache enable */
    int		flags;
} LDAPTCL;


#define LDAPTCL_INTERRCODES	0x001

#include "ldaptclerr.h"

static
LDAP_SetErrorCode(LDAPTCL *ldaptcl, int code, Tcl_Interp *interp)
{
    char shortbuf[6];
    char *errp;
    int   lderrno;

    if (code == -1)
	code = ldap_get_lderrno(ldaptcl->ldap);
    if ((ldaptcl->flags & LDAPTCL_INTERRCODES) || code > LDAPTCL_MAXERR ||
      ldaptclerrorcode[code] == NULL) {
	sprintf(shortbuf, "0x%03x", code);
	errp = shortbuf;
    } else
	errp = ldaptclerrorcode[code];

    Tcl_SetErrorCode(interp, errp, NULL);
}

/*-----------------------------------------------------------------------------
 * LDAP_ProcessOneSearchResult --
 * 
 *   Process one result return from an LDAP search.
 *
 * Paramaters:
 *   o interp -            Tcl interpreter; Errors are returned in result.
 *   o ldap -              LDAP structure pointer.
 *   o entry -             LDAP message pointer.
 *   o destArrayNameObj -  Name of Tcl array in which to store attributes.
 *   o evalCodeObj -       Tcl_Obj pointer to code to eval against this result.
 * Returns:
 *   o TCL_OK if processing succeeded..
 *   o TCL_ERROR if an error occured, with error message in interp.
 *-----------------------------------------------------------------------------
 */
int
LDAP_ProcessOneSearchResult (interp, ldap, entry, destArrayNameObj, evalCodeObj)
    Tcl_Interp     *interp;
    LDAP           *ldap;
    LDAPMessage    *entry;
    Tcl_Obj        *destArrayNameObj;
    Tcl_Obj        *evalCodeObj;
{
    char           *attributeName;
    Tcl_Obj        *attributeNameObj;
    Tcl_Obj        *attributeDataObj;
    int             i; 
    BerElement     *ber; 
    struct berval **bvals;
    char	   *dn;
    int		    lderrno;

    Tcl_UnsetVar (interp, Tcl_GetStringFromObj (destArrayNameObj, NULL), 0);

    dn = ldap_get_dn(ldap, entry);
    if (dn != NULL) {
	if (Tcl_SetVar2(interp,		/* set dn */
		       Tcl_GetStringFromObj(destArrayNameObj, NULL),
		       "dn",
		       dn,
		       TCL_LEAVE_ERR_MSG) == NULL)
	    return TCL_ERROR;
	ldap_memfree(dn);
    }
    for (attributeName = ldap_first_attribute (ldap, entry, &ber); 
      attributeName != NULL;
      attributeName = ldap_next_attribute(ldap, entry, ber)) {

	bvals = ldap_get_values_len(ldap, entry, attributeName);

	if (bvals != NULL) {
	    /* Note here that the U.of.M. ldap will return a null bvals
	       when the last attribute value has been deleted, but still
	       retains the attributeName.  Even though this is documented
	       as an error, we ignore it to present a consistent interface
	       with Netscape's server
	    */
	    attributeNameObj = Tcl_NewStringObj (attributeName, -1);
	    Tcl_IncrRefCount (attributeNameObj);
	    attributeDataObj = Tcl_NewObj();
	    for (i = 0; bvals[i] != NULL; i++) {
		Tcl_Obj *singleAttributeValueObj;

		singleAttributeValueObj = Tcl_NewStringObj (bvals[i]->bv_val, -1);
		if (Tcl_ListObjAppendElement (interp, 
					      attributeDataObj, 
					      singleAttributeValueObj) 
		  == TCL_ERROR) {
		    ldap_ber_free(ber, 0);
		    return TCL_ERROR;
		}
	    }

	    ldap_value_free_len(bvals);

	    if (Tcl_ObjSetVar2 (interp, 
				destArrayNameObj,
				attributeNameObj,
				attributeDataObj,
				TCL_LEAVE_ERR_MSG) == NULL) {
		return TCL_ERROR;
	    }
	    Tcl_DecrRefCount (attributeNameObj);
	}
    }
    ldap_ber_free(ber, 0);
    return Tcl_EvalObj (interp, evalCodeObj);
}

/*-----------------------------------------------------------------------------
 * LDAP_PerformSearch --
 * 
 *   Perform an LDAP search.
 *
 * Paramaters:
 *   o interp -            Tcl interpreter; Errors are returned in result.
 *   o ldap -              LDAP structure pointer.
 *   o base -              Base DN from which to perform search.
 *   o scope -             LDAP search scope, must be one of LDAP_SCOPE_BASE,
 *                         LDAP_SCOPE_ONELEVEL, or LDAP_SCOPE_SUBTREE.
 *   o attrs -             Pointer to array of char * pointers of desired
 *                         attribute names, or NULL for all attributes.
 *   o filtpatt            LDAP filter pattern.
 *   o value               Value to get sprintf'ed into filter pattern.
 *   o destArrayNameObj -  Name of Tcl array in which to store attributes.
 *   o evalCodeObj -       Tcl_Obj pointer to code to eval against this result.
 * Returns:
 *   o TCL_OK if processing succeeded..
 *   o TCL_ERROR if an error occured, with error message in interp.
 *-----------------------------------------------------------------------------
 */
int 
LDAP_PerformSearch (interp, ldaptcl, base, scope, attrs, filtpatt, value,
	destArrayNameObj, evalCodeObj, timeout_p, all, sortattr)
    Tcl_Interp     *interp;
    LDAPTCL        *ldaptcl;
    char           *base;
    int             scope;
    char          **attrs;
    char           *filtpatt;
    char           *value;
    Tcl_Obj        *destArrayNameObj;
    Tcl_Obj        *evalCodeObj;
    struct timeval *timeout_p;
    int		    all;
    char	   *sortattr;
{
    LDAP	 *ldap = ldaptcl->ldap;
    char          filter[BUFSIZ];
    int           resultCode;
    int           errorCode;
    int		  abandon;
    int		  tclResult = TCL_OK;
    int		  msgid;
    LDAPMessage  *resultMessage;
    LDAPMessage  *entryMessage;
    char	  *sortKey;

    Tcl_Obj      *resultObj;
    int		  lderrno;

    resultObj = Tcl_GetObjResult (interp);

    sprintf(filter, filtpatt, value);

    fflush(stderr);
    if ((msgid = ldap_search (ldap, base, scope, filter, attrs, 0)) == -1) {
	Tcl_AppendStringsToObj (resultObj,
			        "LDAP start search error: ",
					LDAP_ERR_STRING(ldap),
			        (char *)NULL);
	LDAP_SetErrorCode(ldaptcl, -1, interp);
	return TCL_ERROR;
    }

    abandon = 0;
    if (sortattr)
	all = 1;
    tclResult = TCL_OK;
    while (!abandon) {
	resultCode = ldap_result (ldap, msgid, all, timeout_p, &resultMessage);
	if (resultCode != LDAP_RES_SEARCH_RESULT &&
	    resultCode != LDAP_RES_SEARCH_ENTRY)
		break;

	if (sortattr) {
	    sortKey = (strcasecmp(sortattr, "dn") == 0) ? NULL : sortattr;
	    ldap_sort_entries(ldap, &resultMessage, sortKey, strcasecmp);
	}
	entryMessage = ldap_first_entry(ldap, resultMessage);

	while (entryMessage) {
	    tclResult = LDAP_ProcessOneSearchResult  (interp, 
				    ldap, 
				    entryMessage,
				    destArrayNameObj,
				    evalCodeObj);
	    if (tclResult != TCL_OK) {
		if (tclResult == TCL_CONTINUE) {
		    tclResult = TCL_OK;
		} else if (tclResult == TCL_BREAK) {
		    tclResult = TCL_OK;
		    abandon = 1;
		    break;
		} else if (tclResult == TCL_ERROR) {
		    char msg[100];
		    sprintf(msg, "\n    (\"search\" body line %d)",
			    interp->errorLine);
		    Tcl_AddObjErrorInfo(interp, msg, -1);
		    abandon = 1;
		    break;
		} else {
		    abandon = 1;
		    break;
		}
	    }
	    entryMessage = ldap_next_entry(ldap, entryMessage);
	}
	if (resultCode == LDAP_RES_SEARCH_RESULT || all)
	    break;
 	ldap_msgfree(resultMessage);
    }
    if (abandon) {
	ldap_msgfree(resultMessage);
	if (resultCode == LDAP_RES_SEARCH_ENTRY)
	    ldap_abandon(ldap, msgid);
	return tclResult;
    }
    if (resultCode == -1) {
	Tcl_AppendStringsToObj (resultObj,
				"LDAP result search error: ",
				LDAP_ERR_STRING(ldap),
				(char *)NULL);
	LDAP_SetErrorCode(ldaptcl, -1, interp);
	return TCL_ERROR;
    }
    if (resultCode == 0) {
	Tcl_SetErrorCode (interp, "TIMEOUT", (char*) NULL);
	Tcl_SetStringObj (resultObj, "LDAP timeout retrieving results", -1);
	return TCL_ERROR;
    }
    /*
    if (resultCode == LDAP_RES_SEARCH_RESULT || 
	(all && resultCode == LDAP_RES_SEARCH_ENTRY))
	    return tclResult;
    */

    if ((errorCode = ldap_result2error (ldap, resultMessage, 0))
      != LDAP_SUCCESS) {
      Tcl_AppendStringsToObj (resultObj,
			      "LDAP search error: ",
			      ldap_err2string(errorCode),
			      (char *)NULL);
      ldap_msgfree(resultMessage);
      LDAP_SetErrorCode(ldaptcl, errorCode, interp);
      return TCL_ERROR;
    }
    return tclResult;
}

/*-----------------------------------------------------------------------------
 * NeoX_LdapTargetObjCmd --
 *  
 * Implements the body of commands created by Neo_LdapObjCmd.
 *  
 * Results:
 *      A standard Tcl result.
 *      
 * Side effects:
 *      See the user documentation.
 *-----------------------------------------------------------------------------
 */     
int
NeoX_LdapTargetObjCmd (clientData, interp, objc, objv)
    ClientData    clientData;
    Tcl_Interp   *interp;
    int           objc;
    Tcl_Obj      *CONST objv[];
{
    char         *command;
    char         *subCommand;
    LDAPTCL      *ldaptcl = (LDAPTCL *)clientData;
    LDAP         *ldap = ldaptcl->ldap;
    char         *dn;
    int           is_add = 0;
    int           is_add_or_modify = 0;
    int           mod_op = 0;
    char	 *m, *s, *errmsg;
    int		 errcode;
    int		 tclResult;

    Tcl_Obj      *resultObj = Tcl_GetObjResult (interp);

    if (objc < 2)
       return TclX_WrongArgs (interp,
			      objv [0],
			      "subcommand [args...]");

    command = Tcl_GetStringFromObj (objv[0], NULL);
    subCommand = Tcl_GetStringFromObj (objv[1], NULL);

    /* object bind authtype name password */
    if (STREQU (subCommand, "bind")) {
	char     *binddn;
	char     *passwd;
	int       stringLength;
	char     *ldap_authString;
	int       ldap_authInt;

	if (objc != 5)
	    return TclX_WrongArgs (interp, objv [0], "bind authtype dn passwd");

	ldap_authString = Tcl_GetStringFromObj (objv[2], NULL);

	if (STREQU (ldap_authString, "simple")) {
	    ldap_authInt = LDAP_AUTH_SIMPLE;
	}
#ifdef UMICH_LDAP
	else if (STREQU (ldap_authString, "kerberos_ldap")) {
	    ldap_authInt = LDAP_AUTH_KRBV41;
	} else if (STREQU (ldap_authString, "kerberos_dsa")) {
	    ldap_authInt = LDAP_AUTH_KRBV42;
	} else if (STREQU (ldap_authString, "kerberos_both")) {
	    ldap_authInt = LDAP_AUTH_KRBV4;
	}
#endif
	else {
	    Tcl_AppendStringsToObj (resultObj,
				    "\"",
				    command,
				    " ",
				    subCommand, 
#ifdef UMICH_LDAP
				    "\" authtype must be one of \"simple\", ",
				    "\"kerberos_ldap\", \"kerberos_dsa\" ",
				    "or \"kerberos_both\"",
#else
				    "\" authtype must be \"simple\", ",
#endif
				    (char *)NULL);
	    return TCL_ERROR;
	}

	binddn = Tcl_GetStringFromObj (objv[3], &stringLength);
	if (stringLength == 0)
	    binddn = NULL;

	passwd = Tcl_GetStringFromObj (objv[4], &stringLength);
	if (stringLength == 0)
	    passwd = NULL;

/*  ldap_bind_s(ldap, dn, pw, method) */

#ifdef UMICH_LDAP
#define LDAP_BIND(ldap, dn, pw, method) \
  ldap_bind_s(ldap, dn, pw, method)
#else
#define LDAP_BIND(ldap, dn, pw, method) \
  ldap_simple_bind_s(ldap, dn, pw)
#endif
	if ((errcode = LDAP_BIND (ldap, 
			 binddn, 
			 passwd, 
			 ldap_authInt)) != LDAP_SUCCESS) {

	    Tcl_AppendStringsToObj (resultObj,
			            "LDAP bind error: ",
				    ldap_err2string(errcode),
				    (char *)NULL);
	    LDAP_SetErrorCode(ldaptcl, errcode, interp);
	    return TCL_ERROR;
	}
	return TCL_OK;
    }

    if (STREQU (subCommand, "unbind")) {
	if (objc != 2)
	    return TclX_WrongArgs (interp, objv [0], "unbind");

       return Tcl_DeleteCommand(interp, Tcl_GetStringFromObj(objv[0], NULL));
    }

    /* object delete dn */
    if (STREQU (subCommand, "delete")) {
	if (objc != 3)
	    return TclX_WrongArgs (interp, objv [0], "delete dn");

       dn = Tcl_GetStringFromObj (objv [2], NULL);
       if ((errcode = ldap_delete_s(ldap, dn)) != LDAP_SUCCESS) {
	   Tcl_AppendStringsToObj (resultObj,
			           "LDAP delete error: ",
				   ldap_err2string(errcode),
				   (char *)NULL);
	   LDAP_SetErrorCode(ldaptcl, errcode, interp);
	   return TCL_ERROR;
       }
       return TCL_OK;
    }

    /* object rename_rdn dn rdn */
    /* object modify_rdn dn rdn */
    if (STREQU (subCommand, "rename_rdn") || STREQU (subCommand, "modify_rdn")) {
	char    *rdn;
	int      deleteOldRdn;

	if (objc != 4)
	    return TclX_WrongArgs (interp, 
				   objv [0], 
				   "delete_rdn|modify_rdn dn rdn");

	dn = Tcl_GetStringFromObj (objv [2], NULL);
	rdn = Tcl_GetStringFromObj (objv [3], NULL);

	deleteOldRdn = (*subCommand == 'r');

	if ((errcode = ldap_modrdn2_s (ldap, dn, rdn, deleteOldRdn)) != LDAP_SUCCESS) {
	    Tcl_AppendStringsToObj (resultObj,
				    "LDAP ",
				    subCommand,
				    " error: ",
				    ldap_err2string(errcode),
				    (char *)NULL);
	    LDAP_SetErrorCode(ldaptcl, errcode, interp);
	    return TCL_ERROR;
	}
	return TCL_OK;
    }

    /* object add dn attributePairList */
    /* object add_attributes dn attributePairList */
    /* object replace_attributes dn attributePairList */
    /* object delete_attributes dn attributePairList */

    if (STREQU (subCommand, "add")) {
	is_add = 1;
	is_add_or_modify = 1;
    } else {
	is_add = 0;
	if (STREQU (subCommand, "add_attributes")) {
	    is_add_or_modify = 1;
	    mod_op = LDAP_MOD_ADD;
	} else if (STREQU (subCommand, "replace_attributes")) {
	    is_add_or_modify = 1;
	    mod_op = LDAP_MOD_REPLACE;
	} else if (STREQU (subCommand, "delete_attributes")) {
	    is_add_or_modify = 1;
	    mod_op = LDAP_MOD_DELETE;
	}
    }

    if (is_add_or_modify) {
	int          result;
	LDAPMod    **modArray;
	LDAPMod     *mod;
	char       **valPtrs = NULL;
	int          attribObjc;
	Tcl_Obj    **attribObjv;
	int          valuesObjc;
	Tcl_Obj    **valuesObjv;
	int          nPairs;
	int          i;
	int          j;

	Tcl_Obj      *resultObj = Tcl_GetObjResult (interp);

	if (objc != 4) {
	    Tcl_AppendStringsToObj (resultObj,
				    "wrong # args: ",
				    Tcl_GetStringFromObj (objv [0], NULL),
				    " ",
				    subCommand,
				    " dn attributePairList",
				    (char *)NULL);
	    return TCL_ERROR;
	}

	dn = Tcl_GetStringFromObj (objv [2], NULL);

	if (Tcl_ListObjGetElements (interp, objv [3], &attribObjc, &attribObjv)
	  == TCL_ERROR) {
	   return TCL_ERROR;
	}

        if (attribObjc & 1) {
	    Tcl_AppendStringsToObj (resultObj,
				    "attribute list does not contain an ",
				    "even number of key-value elements",
				    (char *)NULL);
	    return TCL_ERROR;
	}

	nPairs = attribObjc / 2;

	modArray = (LDAPMod **)ckalloc (sizeof(LDAPMod *) * (nPairs + 1));
	modArray[nPairs] = (LDAPMod *) NULL;

	for (i = 0; i < nPairs; i++) {
	    mod = modArray[i] = (LDAPMod *) ckalloc (sizeof(LDAPMod));
	    mod->mod_op = mod_op;
	    mod->mod_type = Tcl_GetStringFromObj (attribObjv [i * 2], NULL);

	    if (Tcl_ListObjGetElements (interp, attribObjv [i * 2 + 1], &valuesObjc, &valuesObjv) == TCL_ERROR) {
		/* FIX: cleanup memory here */
		return TCL_ERROR;
	    }

	    valPtrs = mod->mod_vals.modv_strvals = \
	        (char **)ckalloc (sizeof (char *) * (valuesObjc + 1));
	    valPtrs[valuesObjc] = (char *)NULL;

	    for (j = 0; j < valuesObjc; j++) {
		valPtrs [j] = Tcl_GetStringFromObj (valuesObjv[j], NULL);

		/* If it's "delete" and value is an empty string, make
		 * value be NULL to indicate entire attribute is to be 
		 * deleted */
		if ((*valPtrs [j] == '\0') 
		    && (mod->mod_op == LDAP_MOD_DELETE)) {
			valPtrs [j] = NULL;
		}
	    }
	}

        if (is_add) {
	    result = ldap_add_s (ldap, dn, modArray);
	} else {
	    result = ldap_modify_s (ldap, dn, modArray);
	    if (ldaptcl->caching)
		ldap_uncache_entry (ldap, dn);
	}

        /* free the modArray elements, then the modArray itself. */
	for (i = 0; i < nPairs; i++) {
	    ckfree ((char *) modArray[i]->mod_vals.modv_strvals);
	    ckfree ((char *) modArray[i]);
	}
	ckfree ((char *) modArray);

	/* FIX: memory cleanup required all over the place here */
        if (result != LDAP_SUCCESS) {
	    Tcl_AppendStringsToObj (resultObj,
				    "LDAP ",
				    subCommand,
				    " error: ",
				    ldap_err2string(result),
				    (char *)NULL);
	    LDAP_SetErrorCode(ldaptcl, result, interp);
	    return TCL_ERROR;
	}
	return TCL_OK;
    }

    /* object search controlArray dn pattern */
    if (STREQU (subCommand, "search")) {
	char        *controlArrayName;
	Tcl_Obj     *controlArrayNameObj;

	char        *scopeString;
	int          scope;

	char        *derefString;
	int          deref;

	char        *baseString;

	char       **attributesArray;
	char        *attributesString;
	int          attributesArgc;

	char        *filterPatternString;

	char	    *timeoutString;
	double 	     timeoutTime;
	struct timeval timeout, *timeout_p;

	char	    *paramString;
	int	     cacheThis = -1;
	int	     all = 0;

	char	    *sortattr;

	Tcl_Obj     *destArrayNameObj;
	Tcl_Obj     *evalCodeObj;

	if (objc != 5)
	    return TclX_WrongArgs (interp, 
				   objv [0],
				   "search controlArray destArray code");

        controlArrayNameObj = objv [2];
	controlArrayName = Tcl_GetStringFromObj (controlArrayNameObj, NULL);

	destArrayNameObj = objv [3];

	evalCodeObj = objv [4];

	baseString = Tcl_GetVar2 (interp, 
				  controlArrayName, 
				  "base",
				  0);

	if (baseString == (char *)NULL) {
	    Tcl_AppendStringsToObj (resultObj,
				    "required element \"base\" ",
				    "is missing from ldap control array \"",
				    controlArrayName,
				    "\"",
				    (char *)NULL);
	    return TCL_ERROR;
	}

	filterPatternString = Tcl_GetVar2 (interp,
				           controlArrayName,
				           "filter",
				           0);
	if (filterPatternString == (char *)NULL) {
	    filterPatternString = "objectclass=*";
	}

	/* Fetch scope setting from control array.
	 * If it doesn't exist, default to subtree scoping.
	 */
	scopeString = Tcl_GetVar2 (interp, controlArrayName, "scope", 0);
	if (scopeString == NULL) {
	    scope = LDAP_SCOPE_SUBTREE;
	} else {
	    if (STREQU(scopeString, "base")) 
		scope = LDAP_SCOPE_BASE;
	    else if (STRNEQU(scopeString, "one", 3))
		scope = LDAP_SCOPE_ONELEVEL;
	    else if (STRNEQU(scopeString, "sub", 3))
		scope = LDAP_SCOPE_SUBTREE;
	    else {
		Tcl_AppendStringsToObj (resultObj,
				        "\"scope\" element of \"",
				        controlArrayName,
				        "\" array is not one of ",
				        "\"base\", \"onelevel\", ",
					"or \"subtree\"",
				      (char *) NULL);
		return TCL_ERROR;
	    }
	}

	/* Fetch dereference control setting from control array.
	 * If it doesn't exist, default to never dereference. */
	derefString = Tcl_GetVar2 (interp,
				   controlArrayName,
				   "deref",
				   0);
				      
	if (derefString == (char *)NULL) {
	    deref = LDAP_DEREF_NEVER;
	} else {
	    if (STREQU(derefString, "never"))
		deref = LDAP_DEREF_NEVER;
	    else if (STREQU(derefString, "search"))
		deref = LDAP_DEREF_SEARCHING;
	    else if (STREQU(derefString, "find") == 0)
		deref = LDAP_DEREF_FINDING;
	    else if (STREQU(derefString, "always"))
		deref = LDAP_DEREF_ALWAYS;
	    else {
		Tcl_AppendStringsToObj (resultObj,
				        "\"deref\" element of \"",
				        controlArrayName,
				        "\" array is not one of ",
				        "\"never\", \"search\", \"find\", ",
				        "or \"always\"",
				        (char *) NULL);
		return TCL_ERROR;
	    }
	}

	/* Fetch list of attribute names from control array.
	 * If entry doesn't exist, default to NULL (all).
	 */
	attributesString = Tcl_GetVar2 (interp,
				        controlArrayName,
				        "attributes", 
				        0);
	if (attributesString == (char *)NULL) {
	    attributesArray = NULL;
	} else {
	    if ((Tcl_SplitList (interp, 
				attributesString,
				&attributesArgc, 
				&attributesArray)) != TCL_OK) {
		return TCL_ERROR;
	    }
	}

	/* Fetch timeout value if there is one
	 */
	timeoutString = Tcl_GetVar2 (interp,
				        controlArrayName,
				        "timeout", 
				        0);
	timeout.tv_usec = 0;
	if (timeoutString == (char *)NULL) {
	    timeout_p = NULL;
	    timeout.tv_sec = 0;
	} else {
	    if (Tcl_GetDouble(interp, timeoutString, &timeoutTime) != TCL_OK)
		return TCL_ERROR;
	    timeout.tv_sec = floor(timeoutTime);
	    timeout.tv_usec = (timeoutTime-timeout.tv_sec) * 1000000;
	    timeout_p = &timeout;
	}

	paramString = Tcl_GetVar2 (interp, controlArrayName, "cache", 0);
	if (paramString) {
	    if (Tcl_GetInt(interp, paramString, &cacheThis) == TCL_ERROR)
		return TCL_ERROR;
	}

	paramString = Tcl_GetVar2 (interp, controlArrayName, "all", 0);
	if (paramString) {
	    if (Tcl_GetInt(interp, paramString, &all) == TCL_ERROR)
		return TCL_ERROR;
	}

	sortattr = Tcl_GetVar2 (interp, controlArrayName, "sort", 0);

#ifdef UMICH_LDAP
	ldap->ld_deref = deref; 
	ldap->ld_timelimit = 0;
	ldap->ld_sizelimit = 0; 
	ldap->ld_options = 0;
#endif

	/* Caching control within the search: if the "cache" control array */
	/* value is set, disable/enable caching accordingly */

	if (cacheThis >= 0 && ldaptcl->caching != cacheThis) {
	    if (cacheThis) {
		if (ldaptcl->timeout == 0) {
		    Tcl_SetStringObj(resultObj, "Caching never before enabled, I have no timeout value to use", -1);
		    return TCL_ERROR;
		}
		ldap_enable_cache(ldap, ldaptcl->timeout, ldaptcl->maxmem);
	    }
	    else
		ldap_disable_cache(ldap);
	}
	tclResult = LDAP_PerformSearch (interp, 
			            ldaptcl, 
			            baseString, 
			            scope, 
			            attributesArray, 
			            filterPatternString, 
			            "",
			            destArrayNameObj,
			            evalCodeObj,
				    timeout_p,
				    all,
				    sortattr);
	/* Following the search, if we changed the caching behavior, change */
	/* it back. */
	if (cacheThis >= 0 && ldaptcl->caching != cacheThis) {
	    if (cacheThis)
		ldap_disable_cache(ldap);
	    else
		ldap_enable_cache(ldap, ldaptcl->timeout, ldaptcl->maxmem);
	}
	return tclResult;
    }

#if defined(UMICH_LDAP) || (defined(OPEN_LDAP) && !defined(LDAP_API_VERSION))
    if (STREQU (subCommand, "cache")) {
	char *cacheCommand;

	if (objc < 3)
	  badargs:
	    return TclX_WrongArgs (interp, 
				   objv [0],
				   "cache command [args...]");

	cacheCommand = Tcl_GetStringFromObj (objv [2], NULL);

	if (STREQU (cacheCommand, "uncache")) {
	    char *dn;

	    if (objc != 4)
		return TclX_WrongArgs (interp, 
				       objv [0],
				       "cache uncache dn");

            dn = Tcl_GetStringFromObj (objv [3], NULL);
	    ldap_uncache_entry (ldap, dn);
	    return TCL_OK;
	}

	if (STREQU (cacheCommand, "enable")) {
	    long   timeout = ldaptcl->timeout;
	    long   maxmem = ldaptcl->maxmem;

	    if (objc > 5)
		return TclX_WrongArgs (interp, 
				       objv [0],
				       "cache enable ?timeout? ?maxmem?");

	    if (objc > 3) {
		if (Tcl_GetLongFromObj (interp, objv [3], &timeout) == TCL_ERROR)
		    return TCL_ERROR;
	    }
	    if (timeout == 0) {
		Tcl_SetStringObj(resultObj,
		    objc > 3 ? "timeouts must be greater than 0" : 
		    "no previous timeout to reference", -1);
		return TCL_ERROR;
	    }

	    if (objc > 4)
		if (Tcl_GetLongFromObj (interp, objv [4], &maxmem) == TCL_ERROR)
		    return TCL_ERROR;

	    if (ldap_enable_cache (ldap, timeout, maxmem) == -1) {
		Tcl_AppendStringsToObj (resultObj,
					"LDAP cache enable error: ",
					LDAP_ERR_STRING(ldap),
					(char *)NULL);
		LDAP_SetErrorCode(ldaptcl, -1, interp);
		return TCL_ERROR;
	    }
	    ldaptcl->caching = 1;
	    ldaptcl->timeout = timeout;
	    ldaptcl->maxmem = maxmem;
	    return TCL_OK;
	}

	if (objc != 3) goto badargs;

	if (STREQU (cacheCommand, "disable")) {
	    ldap_disable_cache (ldap);
	    ldaptcl->caching = 0;
	    return TCL_OK;
	}

	if (STREQU (cacheCommand, "destroy")) {
	    ldap_destroy_cache (ldap);
	    ldaptcl->caching = 0;
	    return TCL_OK;
	}

	if (STREQU (cacheCommand, "flush")) {
	    ldap_flush_cache (ldap);
	    return TCL_OK;
	}

	if (STREQU (cacheCommand, "no_errors")) {
	    ldap_set_cache_options (ldap, LDAP_CACHE_OPT_CACHENOERRS);
	    return TCL_OK;
	}

	if (STREQU (cacheCommand, "all_errors")) {
	    ldap_set_cache_options (ldap, LDAP_CACHE_OPT_CACHEALLERRS);
	    return TCL_OK;
	}

	if (STREQU (cacheCommand, "size_errors")) {
	    ldap_set_cache_options (ldap, 0);
	    return TCL_OK;
	}
	Tcl_AppendStringsToObj (resultObj,
				"\"",
				command,
				" ",
				subCommand, 
				"\" subcommand", 
				" must be one of \"enable\", ",
				"\"disable\", ",
				"\"destroy\", \"flush\", \"uncache\", ",
				"\"no_errors\", \"size_errors\",",
				" or \"all_errors\"",
				(char *)NULL);
	return TCL_ERROR;
    }
#endif
#ifdef LDAP_DEBUG
    if (STREQU (subCommand, "debug")) {
	if (objc != 3) {
	    Tcl_AppendStringsToObj(resultObj, "Wrong # of arguments",
		(char*)NULL);
	    return TCL_ERROR;
	}
	return Tcl_GetIntFromObj(interp, objv[2], &ldap_debug);
    }
#endif

    /* FIX: this needs to enumerate all the possibilities */
    Tcl_AppendStringsToObj (resultObj,
	                    "subcommand \"", 
			    subCommand, 
			    "\" must be one of \"add\", ",
			    "\"add_attributes\", ",
			    "\"bind\", \"cache\", \"delete\", ",
			    "\"delete_attributes\", \"modify\", ",
			    "\"modify_rdn\", \"rename_rdn\", ",
			    "\"replace_attributes\", ",
			    "\"search\" or \"unbind\".",
	                    (char *)NULL);
    return TCL_ERROR;
}

/* 
 * Delete and LDAP command object
 *
 */
static void
NeoX_LdapObjDeleteCmd(clientData)
    ClientData    clientData;
{
    LDAPTCL      *ldaptcl = (LDAPTCL *)clientData;
    LDAP         *ldap = ldaptcl->ldap;

    ldap_unbind(ldap);
    ckfree((char*) ldaptcl);
}

/*-----------------------------------------------------------------------------
 * NeoX_LdapObjCmd --
 *  
 * Implements the `ldap' command:
 *    ldap open newObjName host [port]
 *    ldap init newObjName host [port]
 *  
 * Results:
 *      A standard Tcl result.
 *      
 * Side effects:
 *      See the user documentation.
 *-----------------------------------------------------------------------------
 */     
static int
NeoX_LdapObjCmd (clientData, interp, objc, objv)
    ClientData    clientData;
    Tcl_Interp   *interp;
    int           objc;
    Tcl_Obj      *CONST objv[];
{
    extern int    errno;
    char         *subCommand;
    char         *newCommand;
    char         *ldapHost;
    int           ldapPort = 389;
    LDAP         *ldap;
    LDAPTCL	 *ldaptcl;

    Tcl_Obj      *resultObj = Tcl_GetObjResult (interp);

    if (objc < 3 || objc > 5)
	return TclX_WrongArgs (interp, objv [0],
			       "(open|init) new_command host [port]|explode dn");

    subCommand = Tcl_GetStringFromObj (objv[1], NULL);

    if (STREQU(subCommand, "explode")) {
	char *param;
	int nonames = 0;
	int list = 0;
	char **exploded, **p;

	param = Tcl_GetStringFromObj (objv[2], NULL);
	if (param[0] == '-') {
	    if (STREQU(param, "-nonames")) {
		nonames = 1;
	    } else if (STREQU(param, "-list")) {
		list = 1;
	    } else {
		return TclX_WrongArgs (interp, objv [0], "explode ?-nonames|-list? dn");
	    }
	}
	if (nonames || list)
	    param = Tcl_GetStringFromObj (objv[3], NULL);
	exploded = ldap_explode_dn(param, nonames);
	for (p = exploded; *p; p++) {
	    if (list) {
		char *q = strchr(*p, '=');
		if (!q) {
		    Tcl_SetObjLength(resultObj, 0);
		    Tcl_AppendStringsToObj(resultObj, "rdn ", *p,
			" missing '='", NULL);
		    ldap_value_free(exploded);
		    return TCL_ERROR;
		}
		*q = '\0';
		if (Tcl_ListObjAppendElement(interp, resultObj,
			Tcl_NewStringObj(*p, -1)) != TCL_OK ||
			Tcl_ListObjAppendElement(interp, resultObj,
			Tcl_NewStringObj(q+1, -1)) != TCL_OK) {
		    ldap_value_free(exploded);
		    return TCL_ERROR;
		}
	    } else {
		if (Tcl_ListObjAppendElement(interp, resultObj,
			Tcl_NewStringObj(*p, -1))) {
		    ldap_value_free(exploded);
		    return TCL_ERROR;
		}
	    }
	}
	ldap_value_free(exploded);
	return TCL_OK;
    }

#ifdef UMICH_LDAP
    if (STREQU(subCommand, "friendly")) {
	char *friendly = ldap_dn2ufn(Tcl_GetStringFromObj(objv[2], NULL));
	Tcl_SetStringObj(resultObj, friendly, -1);
	free(friendly);
	return TCL_OK;
    }
#endif

    newCommand = Tcl_GetStringFromObj (objv[2], NULL);
    ldapHost = Tcl_GetStringFromObj (objv[3], NULL);

    if (objc == 5) {
	if (Tcl_GetIntFromObj (interp, objv [4], &ldapPort) == TCL_ERROR) {
	    Tcl_AppendStringsToObj (resultObj,
				    "LDAP port number is non-numeric",
				    (char *)NULL);
            return TCL_ERROR;
	}
    }

    if (STREQU (subCommand, "open")) {
	ldap = ldap_open (ldapHost, ldapPort);
    } else if (STREQU (subCommand, "init")) {
	ldap = ldap_init (ldapHost, ldapPort);
    } else {
	Tcl_AppendStringsToObj (resultObj, 
				"option was not \"open\" or \"init\"");
	return TCL_ERROR;
    }

    if (ldap == (LDAP *)NULL) {
	Tcl_SetErrno(errno);
	Tcl_AppendStringsToObj (resultObj, 
				Tcl_PosixError (interp), 
				(char *)NULL);
	return TCL_ERROR;
    }

#if UMICH_LDAP
    ldap->ld_deref = LDAP_DEREF_NEVER;  /* Turn off alias dereferencing */
#endif

    ldaptcl = (LDAPTCL *) ckalloc(sizeof(LDAPTCL));
    ldaptcl->ldap = ldap;
    ldaptcl->caching = 0;
    ldaptcl->timeout = 0;
    ldaptcl->maxmem = 0;
    ldaptcl->flags = 0;

    Tcl_CreateObjCommand (interp,
			  newCommand,
                          NeoX_LdapTargetObjCmd,
                          (ClientData) ldaptcl,
                          NeoX_LdapObjDeleteCmd);
    return TCL_OK;
}

/*-----------------------------------------------------------------------------
 * Neo_initLDAP --
 *     Initialize the LDAP interface.
 *-----------------------------------------------------------------------------
 */     
int
Ldaptcl_Init (interp)
Tcl_Interp   *interp;
{
    Tcl_CreateObjCommand (interp,
			  "ldap",
                          NeoX_LdapObjCmd,
                          (ClientData) NULL,
                          (Tcl_CmdDeleteProc*) NULL);
    Tcl_PkgProvide(interp, "Ldaptcl", VERSION);
    return TCL_OK;
}
