/* $OpenLDAP$ */
/*
 * Copyright 1998-1999 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */
/*
 * Copyright (c) 1995 Regents of the University of Michigan.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to the University of Michigan at Ann Arbor. The name of the University
 * may not be used to endorse or promote products derived from this
 * software without specific prior written permission. This software
 * is provided ``as is'' without express or implied warranty.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/time.h>
#include <ac/socket.h>

#include "ldap_pvt.h"
#include "slap.h"

#ifdef SLAPD_SCHEMA_NOT_COMPAT
static int slap_mods2entry(
	Modifications *mods,
	Entry **e,
	char **text );
#else
static int	add_created_attrs(Operation *op, Entry *e);
#endif

int
do_add( Connection *conn, Operation *op )
{
	BerElement	*ber = op->o_ber;
	char		*dn, *ndn, *last;
	ber_len_t	len;
	ber_tag_t	tag;
	Entry		*e;
	Backend		*be;
#ifdef SLAPD_SCHEMA_NOT_COMPAT
	LDAPModList	*modlist = NULL;
	LDAPModList	**modtail = &modlist;
	Modifications *mods = NULL;
#endif
	char *text;
	int			rc = LDAP_SUCCESS;

	Debug( LDAP_DEBUG_TRACE, "do_add\n", 0, 0, 0 );

	/*
	 * Parse the add request.  It looks like this:
	 *
	 *	AddRequest := [APPLICATION 14] SEQUENCE {
	 *		name	DistinguishedName,
	 *		attrs	SEQUENCE OF SEQUENCE {
	 *			type	AttributeType,
	 *			values	SET OF AttributeValue
	 *		}
	 *	}
	 */

	/* get the name */
	if ( ber_scanf( ber, "{a", /*}*/ &dn ) == LBER_ERROR ) {
		Debug( LDAP_DEBUG_ANY, "do_add: ber_scanf failed\n", 0, 0, 0 );
		send_ldap_disconnect( conn, op,
			LDAP_PROTOCOL_ERROR, "decoding error" );
		return -1;
	}

	ndn = ch_strdup( dn );

	if ( dn_normalize( ndn ) == NULL ) {
		Debug( LDAP_DEBUG_ANY, "do_add: invalid dn (%s)\n", dn, 0, 0 );
		send_ldap_result( conn, op, LDAP_INVALID_DN_SYNTAX, NULL,
		    "invalid DN", NULL, NULL );
		free( dn );
		free( ndn );
		return LDAP_INVALID_DN_SYNTAX;
	}

	e = (Entry *) ch_calloc( 1, sizeof(Entry) );

	e->e_dn = dn;
	e->e_ndn = ndn;
	e->e_attrs = NULL;
	e->e_private = NULL;

	Debug( LDAP_DEBUG_ARGS, "    do_add: ndn (%s)\n", e->e_ndn, 0, 0 );

	/* get the attrs */
	for ( tag = ber_first_element( ber, &len, &last ); tag != LBER_DEFAULT;
	    tag = ber_next_element( ber, &len, last ) ) {
#ifdef SLAPD_SCHEMA_NOT_COMPAT
		LDAPModList *mod = (LDAPModList *) ch_malloc( sizeof(LDAPModList) );
#else
		LDAPModList tmpmod;
		LDAPModList *mod = &tmpmod;
#endif
		mod->ml_op = LDAP_MOD_ADD;
		mod->ml_next = NULL;

		rc = ber_scanf( ber, "{a{V}}", &mod->ml_type, &mod->ml_bvalues );

		if ( rc == LBER_ERROR ) {
			send_ldap_disconnect( conn, op,
				LDAP_PROTOCOL_ERROR, "decoding error" );
			rc = -1;
#ifdef SLAPD_SCHEMA_NOT_COMPAT
			free( mod );
#endif
			goto done;
		}

		if ( mod->ml_bvalues == NULL ) {
			Debug( LDAP_DEBUG_ANY, "no values for type %s\n",
				mod->ml_type, 0, 0 );
			send_ldap_result( conn, op, rc = LDAP_PROTOCOL_ERROR,
				NULL, "no values for attribute type", NULL, NULL );
			free( mod->ml_type );
#ifdef SLAPD_SCHEMA_NOT_COMPAT
			free( mod );
#endif
			goto done;
		}

#ifdef SLAPD_SCHEMA_NOT_COMPAT
		(*modtail)->ml_next = mod;
		modtail = &mod->ml_next;
#else
		attr_merge( e, mod->ml_type, mod->ml_bvalues );

		free( mod->ml_type );
		ber_bvecfree( mod->ml_bvalues );
#endif
	}

	if ( ber_scanf( ber, /*{*/ "}") == LBER_ERROR ) {
		Debug( LDAP_DEBUG_ANY, "do_add: ber_scanf failed\n", 0, 0, 0 );
		send_ldap_disconnect( conn, op,
			LDAP_PROTOCOL_ERROR, "decoding error" );
		rc = -1;
		goto done;
	}

	if( (rc = get_ctrls( conn, op, 1 )) != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_ANY, "do_add: get_ctrls failed\n", 0, 0, 0 );
		goto done;
	} 

#ifdef SLAPD_SCHEMA_NOT_COMPAT
	if ( modlist == NULL )
#else
	if ( e->e_attrs == NULL )
#endif
	{
		send_ldap_result( conn, op, rc = LDAP_PROTOCOL_ERROR,
			NULL, "no attributes provided", NULL, NULL );
		goto done;
	}

	Statslog( LDAP_DEBUG_STATS, "conn=%ld op=%d ADD dn=\"%s\"\n",
	    op->o_connid, op->o_opid, e->e_ndn, 0, 0 );

	/*
	 * We could be serving multiple database backends.  Select the
	 * appropriate one, or send a referral to our "referral server"
	 * if we don't hold it.
	 */
	be = select_backend( e->e_ndn );
	if ( be == NULL ) {
		send_ldap_result( conn, op, rc = LDAP_REFERRAL,
			NULL, NULL, default_referral, NULL );
		goto done;
	}

	/* make sure this backend recongizes critical controls */
	rc = backend_check_controls( be, conn, op, &text ) ;

	if( rc != LDAP_SUCCESS ) {
		send_ldap_result( conn, op, rc,
			NULL, text, NULL, NULL );
		goto done;
	}

	if ( global_readonly || be->be_readonly ) {
		Debug( LDAP_DEBUG_ANY, "do_add: database is read-only\n",
		       0, 0, 0 );
		send_ldap_result( conn, op, rc = LDAP_UNWILLING_TO_PERFORM,
			NULL, "directory is read-only", NULL, NULL );
		goto done;
	}

	/*
	 * do the add if 1 && (2 || 3)
	 * 1) there is an add function implemented in this backend;
	 * 2) this backend is master for what it holds;
	 * 3) it's a replica and the dn supplied is the updatedn.
	 */
	if ( be->be_add ) {
		/* do the update here */
#ifdef SLAPD_MULTIMASTER
		if ( (be->be_lastmod == ON || (be->be_lastmod == UNDEFINED &&
			global_lastmod == ON)) && (be->be_update_ndn == NULL ||
			strcmp( be->be_update_ndn, op->o_ndn )) )
#else
		if ( be->be_update_ndn == NULL ||
			strcmp( be->be_update_ndn, op->o_ndn ) == 0 )
#endif
		{
			int update = be->be_update_ndn != NULL;

#ifdef SLAPD_SCHEMA_NOT_COMPAT
			rc = slap_modlist2mods( modlist, update, &mods, &text );
			if( rc != LDAP_SUCCESS ) {
				send_ldap_result( conn, op, rc,
					NULL, text, NULL, NULL );
				goto done;
			}

#endif
#ifndef SLAPD_MULTIMASTER
			if ( (be->be_lastmod == ON || (be->be_lastmod == UNDEFINED &&
				global_lastmod == ON)) && !update )
#endif
			{
#ifdef SLAPD_SCHEMA_NOT_COMPAT
				rc = slap_mods_opattrs( op, &mods, &text );
#else
				char *text = "no-user-modification attribute type";
				rc = add_created_attrs( op, e );
#endif
				if( rc != LDAP_SUCCESS ) {
					send_ldap_result( conn, op, rc,
						NULL, text, NULL, NULL );
					goto done;
				}
			}

#ifdef SLAPD_SCHEMA_NOT_COMPAT
			rc = slap_mods2entry( mods, &e, &text );
			if( rc != LDAP_SUCCESS ) {
				send_ldap_result( conn, op, rc,
					NULL, text, NULL, NULL );
				goto done;
			}
#endif

			if ( (*be->be_add)( be, conn, op, e ) == 0 ) {
#ifdef SLAPD_MULTIMASTER
				if (be->be_update_ndn == NULL ||
					strcmp( be->be_update_ndn, op->o_ndn ))
#endif
				{
					replog( be, op, e->e_dn, e );
				}
				be_entry_release_w( be, e );
			}
			e = NULL;

#ifndef SLAPD_MULTIMASTER
		} else {
			send_ldap_result( conn, op, rc = LDAP_REFERRAL, NULL, NULL,
				be->be_update_refs ? be->be_update_refs : default_referral, NULL );
#endif
		}
	} else {
	    Debug( LDAP_DEBUG_ARGS, "    do_add: HHH\n", 0, 0, 0 );
		send_ldap_result( conn, op, rc = LDAP_UNWILLING_TO_PERFORM,
			NULL, "read function not implemented", NULL, NULL );
	}

done:
#ifdef SLAPD_SCHEMA_NOT_COMPAT
	if( modlist != NULL ) {
		slap_modlist_free( modlist );
	}
	if( mods != NULL ) {
		slap_mods_free( mods );
	}
#endif
	if( e != NULL ) {
		entry_free( e );
	}

	return rc;
}

#ifdef SLAPD_SCHEMA_NOT_COMPAT
static int slap_mods2entry(
	Modifications *mods,
	Entry **e,
	char **text )
{
	Attribute **tail = &(*e)->e_attrs;
	assert( *tail == NULL );

	for( ; mods != NULL; mods = mods->sml_next ) {
		Attribute *attr;

		assert( mods->sml_op == LDAP_MOD_ADD );

		attr = attr_find( (*e)->e_attrs, mods->sml_desc );

		if( attr != NULL ) {
			*text = "Attribute provided more than once";
			return LDAP_OPERATIONS_ERROR;
		}

		attr = ch_calloc( 1, sizeof(Attribute) );

		/* should check for duplicates */
		attr->a_vals = mods->sml_bvalues;
		mods->sml_bvalues = NULL;

		*tail = attr;
		tail = &attr->a_next;
	}

	return LDAP_SUCCESS;
}

#else
static int
add_created_attrs( Operation *op, Entry *e )
{
	char		buf[22];
	struct berval	bv;
	struct berval	*bvals[2];
	Attribute	*a;
	struct tm	*ltm;
	time_t		currenttime;

	Debug( LDAP_DEBUG_TRACE, "add_created_attrs\n", 0, 0, 0 );

	bvals[0] = &bv;
	bvals[1] = NULL;

	/* return error on any attempts by the user to add these attrs */
	for ( a = e->e_attrs; a != NULL; a = a->a_next ) {
#ifdef SLAPD_SCHEMA_NOT_COMPAT
		if ( is_at_no_user_mod( a->a_desc.ad_type ))
#else
		if ( oc_check_op_no_usermod_attr( a->a_type ) )
#endif
		{
			return LDAP_CONSTRAINT_VIOLATION;
		}
	}

	if ( op->o_dn == NULL || op->o_dn[0] == '\0' ) {
		bv.bv_val = "<anonymous>";
		bv.bv_len = sizeof("<anonymous>")-1;
;
	} else {
		bv.bv_val = op->o_dn;
		bv.bv_len = strlen( bv.bv_val );
	}
	attr_merge( e, "creatorsname", bvals );
	attr_merge( e, "modifiersname", bvals );

	currenttime = slap_get_time();
	ldap_pvt_thread_mutex_lock( &gmtime_mutex );
	ltm = gmtime( &currenttime );
	strftime( buf, sizeof(buf), "%Y%m%d%H%M%SZ", ltm );
	ldap_pvt_thread_mutex_unlock( &gmtime_mutex );

	bv.bv_val = buf;
	bv.bv_len = strlen( bv.bv_val );
	attr_merge( e, "createtimestamp", bvals );
	attr_merge( e, "modifytimestamp", bvals );

	return LDAP_SUCCESS;
}
#endif
