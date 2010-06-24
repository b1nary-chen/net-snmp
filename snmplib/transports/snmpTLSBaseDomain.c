#include <net-snmp/net-snmp-config.h>

#include <net-snmp/library/snmpTLSBaseDomain.h>

#if HAVE_DMALLOC_H
#include <dmalloc.h>
#endif
#if HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#if HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#if HAVE_NETDB_H
#include <netdb.h>
#endif
#include <errno.h>

/* OpenSSL Includes */
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include <net-snmp/types.h>
#include <net-snmp/library/cert_util.h>
#include <net-snmp/library/snmp_openssl.h>
#include <net-snmp/library/default_store.h>
#include <net-snmp/library/callback.h>
#include <net-snmp/library/snmp_logging.h>
#include <net-snmp/library/snmp_api.h>
#include <net-snmp/library/tools.h>
#include <net-snmp/library/snmp_debug.h>
#include <net-snmp/library/snmp_assert.h>
#include <net-snmp/library/snmp_transport.h>
#include <net-snmp/library/snmp_secmod.h>

#define LOGANDDIE(msg) { snmp_log(LOG_ERR, "%s\n", msg); return 0; }

int openssl_local_index;

/* this is called during negotiationn */
int verify_callback(int ok, X509_STORE_CTX *ctx) {
    int err, depth;
    char buf[1024], *fingerprint;;
    X509 *thecert;
    netsnmp_cert *cert;
    _netsnmp_verify_info *verify_info;
    SSL *ssl;

    thecert = X509_STORE_CTX_get_current_cert(ctx);
    err = X509_STORE_CTX_get_error(ctx);
    depth = X509_STORE_CTX_get_error_depth(ctx);
    
    /* log where we are and why called */
    DEBUGMSGTL(("tls_x509:verify", "verify_callback called with: ok=%d ctx=%p depth=%d err=%i:%s\n", ok, ctx, depth, err, X509_verify_cert_error_string(err)));

    /* things to do: */

    X509_NAME_oneline(X509_get_subject_name(thecert), buf, sizeof(buf));
    fingerprint = netsnmp_openssl_cert_get_fingerprint(thecert, -1);
    DEBUGMSGTL(("tls_x509:verify", "Cert: %s\n", buf));
    DEBUGMSGTL(("tls_x509:verify", "  fp: %s\n", fingerprint ?
                fingerprint : "unknown"));

    ssl = X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
    verify_info = SSL_get_ex_data(ssl, openssl_local_index);

    if (verify_info && ok && depth > 0) {
        /* remember that a parent certificate has been marked as trusted */
        verify_info->parent_was_ok = 1;
    }

    /* this ensures for self-signed certificates we have a valid
       locally known fingerprint and then accept it */
    if (!ok &&
        (X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT == err ||
         X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN == err ||
         X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT == err)) {

        cert = netsnmp_cert_find(NS_CERT_REMOTE_PEER, NS_CERTKEY_FINGERPRINT,
                                 (void*)fingerprint);
        if (cert)
            DEBUGMSGTL(("tls_x509:verify", " Found locally: %s/%s\n",
                        cert->info.dir, cert->info.filename));


        if (cert) {
            DEBUGMSGTL(("tls_x509:verify", "  accepting matching fp of self-signed certificate found in: %s\n",
                        cert->info.filename));
            return 1;
        } else {
            DEBUGMSGTL(("tls_x509:verify", "  no matching fp found\n"));
            return 0;
        }

        if (0 == depth && verify_info && verify_info->parent_was_ok) {
            DEBUGMSGTL(("tls_x509:verify", "  a parent was ok, so returning ok for this child certificate\n"));
            return 1; /* we'll check the hostname later at this level */
        }
    }

    DEBUGMSGTL(("tls_x509:verify", "  returning the passed in value of %d\n",
                ok));
    return(ok);
}

#define VERIFIED_FINGERPRINT      0
#define NO_FINGERPNINT_AVAILABLE  1
#define FAILED_FINGERPRINT_VERIFY 2

static int
_netsnmp_tlsbase_verify_remote_fingerprint(X509 *remote_cert,
                                           _netsnmpTLSBaseData *tlsdata,
                                           int try_default) {

    char            *fingerprint;

    fingerprint =
        netsnmp_openssl_cert_get_fingerprint(remote_cert, -1);

    if (!fingerprint) {
        /* no peer cert */
        snmp_log(LOG_ERR, "failed to get fingerprint of remote certificate\n");
        return FAILED_FINGERPRINT_VERIFY;
    }

    if (!tlsdata->their_fingerprint && tlsdata->their_identity) {
        /* we have an identity; try and find it's fingerprint */
        netsnmp_cert *their_cert;
        their_cert =
            netsnmp_cert_find(NS_CERT_REMOTE_PEER, NS_CERTKEY_MULTIPLE,
                              tlsdata->their_identity);

        if (their_cert)
            tlsdata->their_fingerprint =
                netsnmp_openssl_cert_get_fingerprint(their_cert->ocert, -1);
    }

    if (!tlsdata->their_fingerprint && try_default) {
        /* try for the default instead */
        netsnmp_cert *their_cert;
        their_cert =
            netsnmp_cert_find(NS_CERT_REMOTE_PEER, NS_CERTKEY_DEFAULT,
                              NULL);

        if (their_cert)
            tlsdata->their_fingerprint =
                netsnmp_openssl_cert_get_fingerprint(their_cert->ocert, -1);
    }
    
    if (tlsdata->their_fingerprint) {
        netsnmp_fp_lowercase_and_strip_colon(tlsdata->their_fingerprint);
        if (0 != strcmp(tlsdata->their_fingerprint, fingerprint)) {
            snmp_log(LOG_ERR, "The fingerprint from the remote side's certificate didn't match the expected\n");
            snmp_log(LOG_ERR, "  %s != %s\n",
                     fingerprint, tlsdata->their_fingerprint);
            free(fingerprint);
            return FAILED_FINGERPRINT_VERIFY;
        }
    } else {
        DEBUGMSGTL(("tls_x509:verify", "No fingerprint for the remote entity available to verify\n"));
        return NO_FINGERPNINT_AVAILABLE;
    }

    free(fingerprint);
    return VERIFIED_FINGERPRINT;
}

/* this is called after the connection on the client side by us to check
   other aspects about the connection */
int
netsnmp_tlsbase_verify_server_cert(SSL *ssl, _netsnmpTLSBaseData *tlsdata) {
    /* XXX */
    X509            *remote_cert;
    char            *common_name;
    int              ret;
    
    netsnmp_assert_or_return(ssl != NULL, SNMPERR_GENERR);
    netsnmp_assert_or_return(tlsdata != NULL, SNMPERR_GENERR);
    
    if (NULL == (remote_cert = SSL_get_peer_certificate(ssl))) {
        /* no peer cert */
        DEBUGMSGTL(("tls_x509:verify",
                    "remote connection provided no certificate (yet)\n"));
        return SNMPERR_TLS_NO_CERTIFICATE;
    }

    /* make sure that the fingerprint matches */
    ret = _netsnmp_tlsbase_verify_remote_fingerprint(remote_cert, tlsdata, 1);
    switch(ret) {
    case VERIFIED_FINGERPRINT:
        return SNMPERR_SUCCESS;

    case FAILED_FINGERPRINT_VERIFY:
        return SNMPERR_GENERR;

    case NO_FINGERPNINT_AVAILABLE:
        if (tlsdata->their_hostname) {
            /* if the hostname we were expecting to talk to matches
               the cert, then we can accept this connection. */

            /* check the common name for a match */
            common_name =
                netsnmp_openssl_cert_get_commonName(remote_cert, NULL, NULL);

            if (tlsdata->their_hostname[0] != '\0' &&
                strcmp(tlsdata->their_hostname, common_name) == 0) {
                DEBUGMSGTL(("tls_x509:verify", "Successful match on a common name of %s\n", common_name));
                return SNMPERR_SUCCESS;
            }
        }
        /* XXX: check for hostname match instead */
        snmp_log(LOG_ERR, "Can not verify a remote server identity without configuration\n");
        return SNMPERR_GENERR;
    }
    DEBUGMSGTL(("tls_x509:verify", "shouldn't get here\n"));
    return SNMPERR_GENERR;
}

/* this is called after the connection on the server side by us to check
   the validity of the client's certificate */
int
netsnmp_tlsbase_verify_client_cert(SSL *ssl, _netsnmpTLSBaseData *tlsdata) {
    /* XXX */
    X509            *remote_cert;
    int ret;

    if (NULL == (remote_cert = SSL_get_peer_certificate(ssl))) {
        /* no peer cert */
        DEBUGMSGTL(("tls_x509:verify",
                    "remote connection provided no certificate (yet)\n"));
        return SNMPERR_TLS_NO_CERTIFICATE;
    }

    /* we don't force a known remote fingerprint for a client since we
       will accept any certificate we know about (and later derive the
       securityName from it and apply access control) */
    ret = _netsnmp_tlsbase_verify_remote_fingerprint(remote_cert, tlsdata, 0);
    switch(ret) {
    case FAILED_FINGERPRINT_VERIFY:
        DEBUGMSGTL(("tls_x509:verify", "failed to verify a client fingerprint\n"));
        return SNMPERR_GENERR;

    case NO_FINGERPNINT_AVAILABLE:
        DEBUGMSGTL(("tls_x509:verify", "no known fingerprint available (not a failure case)\n"));
        return SNMPERR_SUCCESS;

    case VERIFIED_FINGERPRINT:
        DEBUGMSGTL(("tls_x509:verify", "Verified client fingerprint\n"));
        tlsdata->flags |= NETSNMP_TLSBASE_CERT_FP_VERIFIED;
        return SNMPERR_SUCCESS;
    }

    DEBUGMSGTL(("tls_x509:verify", "shouldn't get here\n"));
    return SNMPERR_GENERR;
}

/* this is called after the connection on the server side by us to
   check other aspects about the connection and obtain the
   securityName from the remote certificate. */
int
netsnmp_tlsbase_extract_security_name(SSL *ssl, _netsnmpTLSBaseData *tlsdata) {
    netsnmp_container  *chain_maps;
    netsnmp_cert_map   *cert_map, *peer_cert;
    netsnmp_iterator  *itr;
    int                 rc;

    netsnmp_assert_or_return(ssl != NULL, SNMPERR_GENERR);
    netsnmp_assert_or_return(tlsdata != NULL, SNMPERR_GENERR);

    if (NULL == (chain_maps = netsnmp_openssl_get_cert_chain(ssl)))
        return SNMPERR_GENERR;
    /*
     * map fingerprints to mapping entries
     */
    rc = netsnmp_cert_get_secname_maps(chain_maps);
    if ((-1 == rc) || (CONTAINER_SIZE(chain_maps) == 0)) {
        netsnmp_cert_map_container_free(chain_maps);
        return SNMPERR_GENERR;
    }

    /*
     * change container to sorted (by clearing unsorted option), then
     * iterate over it until we find a map that returns a secname.
     */
    CONTAINER_SET_OPTIONS(chain_maps, 0, rc);
    itr = CONTAINER_ITERATOR(chain_maps);
    if (NULL == itr) {
        snmp_log(LOG_ERR, "could not get iterator for secname fingerprints\n");
        netsnmp_cert_map_container_free(chain_maps);
        return SNMPERR_GENERR;
    }
    peer_cert = cert_map = ITERATOR_FIRST(itr);
    for( ; !tlsdata->securityName && cert_map; cert_map = ITERATOR_NEXT(itr))
        tlsdata->securityName =
            netsnmp_openssl_extract_secname(cert_map, peer_cert);
    ITERATOR_RELEASE(itr);

    netsnmp_cert_map_container_free(chain_maps);
       
    return (tlsdata->securityName ? SNMPERR_SUCCESS : SNMPERR_GENERR);
}

int
_trust_this_cert(SSL_CTX *the_ctx, char *certspec) {
    netsnmp_cert *trustcert;

    DEBUGMSGTL(("sslctx_client", "Trying to load a trusted certificate: %s\n",
                certspec));

    /* load this identifier into the trust chain */
    trustcert = netsnmp_cert_find(NS_CERT_CA,
                                  NS_CERTKEY_MULTIPLE,
                                  certspec);
    if (!trustcert)
        trustcert = netsnmp_cert_find(NS_CERT_REMOTE_PEER,
                                      NS_CERTKEY_MULTIPLE,
                                      certspec);
    if (!trustcert)
        LOGANDDIE("failed to find requested certificate to trust");
        
    if (netsnmp_cert_trust_ca(the_ctx, trustcert) != SNMPERR_SUCCESS)
        LOGANDDIE("failed to load trust certificate");

    return 1;
}

void
_load_trusted_certs(SSL_CTX *the_ctx) {
    netsnmp_container *trusted_certs = NULL;
    netsnmp_iterator  *trusted_cert_iterator = NULL;
    char *fingerprint;

    trusted_certs = netsnmp_cert_get_trustlist();
    trusted_cert_iterator = CONTAINER_ITERATOR(trusted_certs);
    if (trusted_cert_iterator) {
        for (fingerprint = (char *) ITERATOR_FIRST(trusted_cert_iterator);
             fingerprint; fingerprint = ITERATOR_NEXT(trusted_cert_iterator)) {
            if (!_trust_this_cert(the_ctx, fingerprint))
                snmp_log(LOG_ERR, "failed to load trust cert: %s\n",
                         fingerprint);
        }
    }
}    

SSL_CTX *
sslctx_client_setup(SSL_METHOD *method, _netsnmpTLSBaseData *tlsbase) {
    netsnmp_cert *id_cert, *peer_cert;
    SSL_CTX      *the_ctx;
    X509_STORE   *cert_store = NULL;

    /***********************************************************************
     * Set up the client context
     */
    the_ctx = SSL_CTX_new(method);
    if (!the_ctx) {
        snmp_log(LOG_ERR, "ack: %p\n", the_ctx);
        LOGANDDIE("can't create a new context");
    }
    SSL_CTX_set_read_ahead (the_ctx, 1); /* Required for DTLS */
        
    SSL_CTX_set_verify(the_ctx,
                       SSL_VERIFY_PEER|
                       SSL_VERIFY_FAIL_IF_NO_PEER_CERT|
                       SSL_VERIFY_CLIENT_ONCE,
                       &verify_callback);

    if (tlsbase->our_identity) {
        DEBUGMSGTL(("sslctx_client", "looking for local id: %s\n", tlsbase->our_identity));
        id_cert = netsnmp_cert_find(NS_CERT_IDENTITY, NS_CERTKEY_MULTIPLE,
                                    tlsbase->our_identity);
    } else {
        DEBUGMSGTL(("sslctx_client", "looking for default local id: %s\n", tlsbase->our_identity));
        id_cert = netsnmp_cert_find(NS_CERT_IDENTITY, NS_CERTKEY_DEFAULT, NULL);
    }

    if (!id_cert)
        LOGANDDIE ("error finding client identity keys");

    if (!id_cert->key || !id_cert->key->okey)
        LOGANDDIE("failed to load private key");

    DEBUGMSGTL(("sslctx_client", "using public key: %s\n",
                id_cert->info.filename));
    DEBUGMSGTL(("sslctx_client", "using private key: %s\n",
                id_cert->key->info.filename));

    if (SSL_CTX_use_certificate(the_ctx, id_cert->ocert) <= 0)
        LOGANDDIE("failed to set the certificate to use");

    if (SSL_CTX_use_PrivateKey(the_ctx, id_cert->key->okey) <= 0)
        LOGANDDIE("failed to set the private key to use");

    if (!SSL_CTX_check_private_key(the_ctx))
        LOGANDDIE("public and private keys incompatible");

    cert_store = SSL_CTX_get_cert_store(the_ctx);
    if (!cert_store)
        LOGANDDIE("failed to find certificate store");

    if (tlsbase->their_identity)
        peer_cert = netsnmp_cert_find(NS_CERT_REMOTE_PEER,
                                      NS_CERTKEY_MULTIPLE,
                                      tlsbase->their_identity);
    else
        peer_cert = netsnmp_cert_find(NS_CERT_REMOTE_PEER, NS_CERTKEY_DEFAULT,
                                      NULL);
    if (peer_cert) {
        DEBUGMSGTL(("sslctx_client", "server's expected public key: %s\n",
                    peer_cert ? peer_cert->info.filename : "none"));

        /* Trust the expected certificate */
        if (netsnmp_cert_trust_ca(the_ctx, peer_cert) != SNMPERR_SUCCESS)
            LOGANDDIE ("failed to set verify paths");
    }

    /* trust a certificate (possibly a CA) aspecifically passed in */
    if (tlsbase->trust_cert) {
        if (!_trust_this_cert(the_ctx, tlsbase->trust_cert))
            return 0;
    }

    _load_trusted_certs(the_ctx);

    if (!SSL_CTX_set_default_verify_paths(the_ctx)) {
        LOGANDDIE ("");
    }

    return the_ctx;
}

SSL_CTX *
sslctx_server_setup(const SSL_METHOD *method) {
    netsnmp_cert *id_cert;

    /***********************************************************************
     * Set up the server context
     */
    /* setting up for ssl */
    SSL_CTX *the_ctx = SSL_CTX_new(method);
    if (!the_ctx) {
        LOGANDDIE("can't create a new context");
    }

    id_cert = netsnmp_cert_find(NS_CERT_IDENTITY, NS_CERTKEY_DEFAULT,
                                (void*)1);
    if (!id_cert)
        LOGANDDIE ("error finding server identity keys");

    if (!id_cert->key || !id_cert->key->okey)
        LOGANDDIE("failed to load private key");

    DEBUGMSGTL(("sslctx_server", "using public key: %s\n",
                id_cert->info.filename));
    DEBUGMSGTL(("sslctx_server", "using private key: %s\n",
                id_cert->key->info.filename));

    if (SSL_CTX_use_certificate(the_ctx, id_cert->ocert) <= 0)
        LOGANDDIE("failed to set the certificate to use");

    if (SSL_CTX_use_PrivateKey(the_ctx, id_cert->key->okey) <= 0)
        LOGANDDIE("failed to set the private key to use");

    if (!SSL_CTX_check_private_key(the_ctx))
        LOGANDDIE("public and private keys incompatible");

    SSL_CTX_set_read_ahead(the_ctx, 1); /* XXX: DTLS only? */

    SSL_CTX_set_verify(the_ctx,
                       SSL_VERIFY_PEER|
                       SSL_VERIFY_FAIL_IF_NO_PEER_CERT|
                       SSL_VERIFY_CLIENT_ONCE,
                       &verify_callback);

    _load_trusted_certs(the_ctx);

    return the_ctx;
}

int
netsnmp_tlsbase_config(struct netsnmp_transport_s *t, const char *token, const char *value) {
    _netsnmpTLSBaseData *tlsdata;

    netsnmp_assert_or_return(t != NULL, -1);
    netsnmp_assert_or_return(t->data != NULL, -1);

    tlsdata = t->data;

    if (strcmp(token, "our_identity") == 0) {
        SNMP_FREE(tlsdata->our_identity);
        tlsdata->our_identity = strdup(value);
        DEBUGMSGT(("tls:config","our identity %s\n", value));
    }

    if (strcmp(token, "their_identity") == 0) {
        SNMP_FREE(tlsdata->their_identity);
        tlsdata->their_identity = strdup(value);
        DEBUGMSGT(("tls:config","their identity %s\n", value));
    }

    if (strcmp(token, "their_hostname") == 0) {
        SNMP_FREE(tlsdata->their_hostname);
        tlsdata->their_hostname = strdup(value);
    }

    if (strcmp(token, "trust_cert") == 0) {
        SNMP_FREE(tlsdata->trust_cert);
        tlsdata->trust_cert = strdup(value);
    }
    
    return SNMPERR_SUCCESS;
}

int
netsnmp_tlsbase_session_init(struct netsnmp_transport_s *transport,
                             struct snmp_session *sess) {
    /* the default security model here should be TSM; most other
       things won't work with TLS because we'll throw out the packet
       if it doesn't have a proper tmStateRef (and onyl TSM generates
       this at the moment */
    if (!(transport->flags & NETSNMP_TRANSPORT_FLAG_LISTEN)) {
        if (sess->securityModel == SNMP_DEFAULT_SECMODEL) {
            sess->securityModel = SNMP_SEC_MODEL_TSM;
        } else if (sess->securityModel != SNMP_SEC_MODEL_TSM) {
            sess->securityModel = SNMP_SEC_MODEL_TSM;
            snmp_log(LOG_ERR, "A security model other than TSM is being used with (D)TLS; using TSM anyways\n");
        }

        if (NULL == sess->securityName) {
            /* client side doesn't need a real securityName */
            /* XXX: call-home issues require them to set one for VACM; but
               we don't do callhome yet */
            sess->securityName = strdup("__BOGUS__");
            sess->securityNameLen = strlen(sess->securityName);
        }

        if (sess->version != SNMP_VERSION_3) {
            sess->version = SNMP_VERSION_3;
            snmp_log(LOG_ERR, "A SNMP version other than 3 was requested with (D)TLS; using 3 anyways\n");
        }

        if (sess->securityLevel <= 0) {
            sess->securityLevel = SNMP_SEC_LEVEL_AUTHPRIV;
        }
    }
    return SNMPERR_SUCCESS;
}

static int have_done_bootstrap = 0;

static int
tls_bootstrap(int majorid, int minorid, void *serverarg, void *clientarg) {
    /* don't do this more than once */
    if (have_done_bootstrap)
        return 0;
    have_done_bootstrap = 1;

    netsnmp_certs_load();

    openssl_local_index =
        SSL_get_ex_new_index(0, "_netsnmp_verify_info", NULL, NULL, NULL);

    return 0;
}

int
tls_get_verify_info_index() {
    return openssl_local_index;
}

void
netsnmp_tlsbase_ctor(void) {

    /* bootstrap ssl since we'll need it */
    netsnmp_init_openssl();

    /* the private client cert to authenticate with */
    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "extraX509SubDir",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_CERT_EXTRA_SUBDIR);

    /*
     * for the client
     */

    /* pem file of valid server CERT CAs */
    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "defX509ServerCerts",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_X509_SERVER_CERTS);

    /* the public client cert to authenticate with */
    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "defX509ClientPub",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_X509_CLIENT_PUB);

    /* the private client cert to authenticate with */
    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "defX509ClientPriv",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_X509_CLIENT_PRIV);

    /*
     * for the server
     */

    /* The list of valid client keys to accept (or CAs I think) */
    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "defX509ClientCerts",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_X509_CLIENT_CERTS);

    /* The X509 server key to use */
    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "defX509ServerPub",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_X509_SERVER_PUB);

    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "defX509ServerPriv",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_X509_SERVER_PRIV);

    /*
     * register our boot-strapping needs
     */
    snmp_register_callback(SNMP_CALLBACK_LIBRARY,
			   SNMP_CALLBACK_POST_PREMIB_READ_CONFIG,
			   tls_bootstrap, NULL);

}

_netsnmpTLSBaseData *
netsnmp_tlsbase_allocate_tlsdata(netsnmp_transport *t, int isserver) {

    _netsnmpTLSBaseData *tlsdata;

    if (NULL == t)
        return NULL;

    /* allocate our TLS specific data */
    tlsdata = SNMP_MALLOC_TYPEDEF(_netsnmpTLSBaseData);
    if (NULL == tlsdata) {
        SNMP_FREE(t);
        return NULL;
    }

    if (!isserver)
        tlsdata->flags |= NETSNMP_TLSBASE_IS_CLIENT;

    return tlsdata;
}


int netsnmp_tlsbase_wrapup_recv(netsnmp_tmStateReference *tmStateRef,
                                _netsnmpTLSBaseData *tlsdata,
                                void **opaque, int *olength) {
    /* RFCXXXX Section 5.1.2 step 2: tmSecurityLevel */
    /* XXX: disallow NULL auth/encr algs in our implementations */
    tmStateRef->transportSecurityLevel = SNMP_SEC_LEVEL_AUTHPRIV;

    /* use x509 cert to do lookup to secname if DNE in cachep yet */
    if (!tlsdata->securityName) {
        netsnmp_tlsbase_extract_security_name(tlsdata->ssl, tlsdata);
        if (NULL != tlsdata->securityName) {
            DEBUGMSGTL(("tls", "set SecName to: %s\n", tlsdata->securityName));
        } else {
            SNMP_FREE(tmStateRef);
            return SNMPERR_GENERR;
        }
    }

    /* RFCXXXX Section 5.1.2 step 2: tmSecurityName */
    /* XXX: detect and throw out overflow secname sizes rather
       than truncating. */
    strncpy(tmStateRef->securityName, tlsdata->securityName,
            sizeof(tmStateRef->securityName)-1);
    tmStateRef->securityName[sizeof(tmStateRef->securityName)-1] = '\0';
    tmStateRef->securityNameLen = strlen(tmStateRef->securityName);

    /* RFCXXXX Section 5.1.2 step 2: tmSessionID */
    /* use our TLSData pointer as the session ID */
    memcpy(tmStateRef->sessionID, &tlsdata, sizeof(netsnmp_tmStateReference *));

    /* save the tmStateRef in our special pointer */
    *opaque = tmStateRef;
    *olength = sizeof(netsnmp_tmStateReference);

    return SNMPERR_SUCCESS;
}

const char * _x509_get_error(int x509failvalue, const char *location) {
    static const char *reason = NULL;
    
    /* XXX: use this instead: X509_verify_cert_error_string(err) */

    switch (x509failvalue) {
    case X509_V_OK:
        reason = "X509_V_OK";
        break;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
        reason = "X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT";
        break;
    case X509_V_ERR_UNABLE_TO_GET_CRL:
        reason = "X509_V_ERR_UNABLE_TO_GET_CRL";
        break;
    case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE:
        reason = "X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE";
        break;
    case X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE:
        reason = "X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE";
        break;
    case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY:
        reason = "X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY";
        break;
    case X509_V_ERR_CERT_SIGNATURE_FAILURE:
        reason = "X509_V_ERR_CERT_SIGNATURE_FAILURE";
        break;
    case X509_V_ERR_CRL_SIGNATURE_FAILURE:
        reason = "X509_V_ERR_CRL_SIGNATURE_FAILURE";
        break;
    case X509_V_ERR_CERT_NOT_YET_VALID:
        reason = "X509_V_ERR_CERT_NOT_YET_VALID";
        break;
    case X509_V_ERR_CERT_HAS_EXPIRED:
        reason = "X509_V_ERR_CERT_HAS_EXPIRED";
        break;
    case X509_V_ERR_CRL_NOT_YET_VALID:
        reason = "X509_V_ERR_CRL_NOT_YET_VALID";
        break;
    case X509_V_ERR_CRL_HAS_EXPIRED:
        reason = "X509_V_ERR_CRL_HAS_EXPIRED";
        break;
    case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
        reason = "X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD";
        break;
    case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
        reason = "X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD";
        break;
    case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD:
        reason = "X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD";
        break;
    case X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD:
        reason = "X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD";
        break;
    case X509_V_ERR_OUT_OF_MEM:
        reason = "X509_V_ERR_OUT_OF_MEM";
        break;
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        reason = "X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT";
        break;
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
        reason = "X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN";
        break;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        reason = "X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY";
        break;
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
        reason = "X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE";
        break;
    case X509_V_ERR_CERT_CHAIN_TOO_LONG:
        reason = "X509_V_ERR_CERT_CHAIN_TOO_LONG";
        break;
    case X509_V_ERR_CERT_REVOKED:
        reason = "X509_V_ERR_CERT_REVOKED";
        break;
    case X509_V_ERR_INVALID_CA:
        reason = "X509_V_ERR_INVALID_CA";
        break;
    case X509_V_ERR_PATH_LENGTH_EXCEEDED:
        reason = "X509_V_ERR_PATH_LENGTH_EXCEEDED";
        break;
    case X509_V_ERR_INVALID_PURPOSE:
        reason = "X509_V_ERR_INVALID_PURPOSE";
        break;
    case X509_V_ERR_CERT_UNTRUSTED:
        reason = "X509_V_ERR_CERT_UNTRUSTED";
        break;
    case X509_V_ERR_CERT_REJECTED:
        reason = "X509_V_ERR_CERT_REJECTED";
        break;
    case X509_V_ERR_SUBJECT_ISSUER_MISMATCH:
        reason = "X509_V_ERR_SUBJECT_ISSUER_MISMATCH";
        break;
    case X509_V_ERR_AKID_SKID_MISMATCH:
        reason = "X509_V_ERR_AKID_SKID_MISMATCH";
        break;
    case X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH:
        reason = "X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH";
        break;
    case X509_V_ERR_KEYUSAGE_NO_CERTSIGN:
        reason = "X509_V_ERR_KEYUSAGE_NO_CERTSIGN";
        break;
    case X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER:
        reason = "X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER";
        break;
    case X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION:
        reason = "X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION";
        break;
    case X509_V_ERR_KEYUSAGE_NO_CRL_SIGN:
        reason = "X509_V_ERR_KEYUSAGE_NO_CRL_SIGN";
        break;
    case X509_V_ERR_UNHANDLED_CRITICAL_CRL_EXTENSION:
        reason = "X509_V_ERR_UNHANDLED_CRITICAL_CRL_EXTENSION";
        break;
    case X509_V_ERR_INVALID_NON_CA:
        reason = "X509_V_ERR_INVALID_NON_CA";
        break;
    case X509_V_ERR_PROXY_PATH_LENGTH_EXCEEDED:
        reason = "X509_V_ERR_PROXY_PATH_LENGTH_EXCEEDED";
        break;
    case X509_V_ERR_KEYUSAGE_NO_DIGITAL_SIGNATURE:
        reason = "X509_V_ERR_KEYUSAGE_NO_DIGITAL_SIGNATURE";
        break;
    case X509_V_ERR_PROXY_CERTIFICATES_NOT_ALLOWED:
        reason = "X509_V_ERR_PROXY_CERTIFICATES_NOT_ALLOWED";
        break;
    case X509_V_ERR_INVALID_EXTENSION:
        reason = "X509_V_ERR_INVALID_EXTENSION";
        break;
    case X509_V_ERR_INVALID_POLICY_EXTENSION:
        reason = "X509_V_ERR_INVALID_POLICY_EXTENSION";
        break;
    case X509_V_ERR_NO_EXPLICIT_POLICY:
        reason = "X509_V_ERR_NO_EXPLICIT_POLICY";
        break;
    case X509_V_ERR_UNNESTED_RESOURCE:
        reason = "X509_V_ERR_UNNESTED_RESOURCE";
        break;
    case X509_V_ERR_APPLICATION_VERIFICATION:
        reason = "X509_V_ERR_APPLICATION_VERIFICATION";
    default:
        reason = "unknown failure code";
    }

    return reason;
}

void _openssl_log_error(int rc, SSL *con, const char *location) {
    const char     *reason, *file, *data;
    unsigned long   numerical_reason;
    int             flags, line;

    snmp_log(LOG_ERR, "---- OpenSSL Related Erorrs: ----\n");

    /* SSL specific errors */
    if (con) {

        int sslnum = SSL_get_error(con, rc);

        switch(sslnum) {
        case SSL_ERROR_NONE:
            reason = "SSL_ERROR_NONE";
            break;

        case SSL_ERROR_SSL:
            reason = "SSL_ERROR_SSL";
            break;

        case SSL_ERROR_WANT_READ:
            reason = "SSL_ERROR_WANT_READ";
            break;

        case SSL_ERROR_WANT_WRITE:
            reason = "SSL_ERROR_WANT_WRITE";
            break;

        case SSL_ERROR_WANT_X509_LOOKUP:
            reason = "SSL_ERROR_WANT_X509_LOOKUP";
            break;

        case SSL_ERROR_SYSCALL:
            reason = "SSL_ERROR_SYSCALL";
            snmp_log(LOG_ERR, "TLS error: %s: rc=%d, sslerror = %d (%s): system_error=%d (%s)\n",
                     location, rc, sslnum, reason, errno, strerror(errno));
            snmp_log(LOG_ERR, "TLS Error: %s\n",
                     ERR_reason_error_string(ERR_get_error()));
            return;

        case SSL_ERROR_ZERO_RETURN:
            reason = "SSL_ERROR_ZERO_RETURN";
            break;

        case SSL_ERROR_WANT_CONNECT:
            reason = "SSL_ERROR_WANT_CONNECT";
            break;

        case SSL_ERROR_WANT_ACCEPT:
            reason = "SSL_ERROR_WANT_ACCEPT";
            break;
            
        default:
            reason = "unknown";
        }

        snmp_log(LOG_ERR, " TLS error: %s: rc=%d, sslerror = %d (%s)\n",
                 location, rc, sslnum, reason);

        snmp_log(LOG_ERR, " TLS Error: %s\n",
                 ERR_reason_error_string(ERR_get_error()));

    }

    /* other errors */
    while ((numerical_reason =
            ERR_get_error_line_data(&file, &line, &data, &flags)) != 0) {
        snmp_log(LOG_ERR, " error: #%lu (file %s, line %d)\n",
                 numerical_reason, file, line);

        /* if we have a text translation: */
        if (data && (flags & ERR_TXT_STRING)) {
            snmp_log(LOG_ERR, "  Textual Error: %s\n", data);
            /*
             * per openssl man page: If it has been allocated by
             * OPENSSL_malloc(), *flags&ERR_TXT_MALLOCED is true.
             *
             * arggh... stupid openssl prototype for ERR_get_error_line_data
             * wants a const char **, but returns something that we might
             * need to free??
             */
            if (flags & ERR_TXT_MALLOCED)
                OPENSSL_free(NETSNMP_REMOVE_CONST(void *, data));        }
    }
    
    snmp_log(LOG_ERR, "---- End of OpenSSL Errors ----\n");
}