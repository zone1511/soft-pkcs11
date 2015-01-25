/*
 * Copyright (c) 2004-2006, Stockholms universitet
 * (Stockholm University, Stockholm Sweden)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the university nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "locl.h"

/* RCSID("$Id: main.c,v 1.24 2006/01/11 12:42:53 lha Exp $"); */

#define OBJECT_ID_MASK		0xfff
#define HANDLE_OBJECT_ID(h)	((h) & OBJECT_ID_MASK)
#define OBJECT_ID(obj)		HANDLE_OBJECT_ID((obj)->object_handle)

struct st_attr {
    CK_ATTRIBUTE attribute;
    int secret;
};

struct st_object {
    CK_OBJECT_HANDLE object_handle;
    struct st_attr *attrs;
    int num_attributes;
    enum {
	STO_T_CERTIFICATE,
	STO_T_PRIVATE_KEY,
	STO_T_PUBLIC_KEY
    } type;
    union {
	X509 *cert;
	EVP_PKEY *public_key;
	struct {
	    const char *file;
	    EVP_PKEY *key;
	    X509 *cert;
	} private_key;
    } u;
};

static struct soft_token {
    CK_VOID_PTR application;
    CK_NOTIFY notify;
    struct {
	struct st_object **objs;
	int num_objs;
    } object;
    struct {
	int hardware_slot;
	int app_error_fatal;
	int login_done;
    } flags;
    int open_sessions;
    struct session_state {
	CK_SESSION_HANDLE session_handle;

	struct {
	    CK_ATTRIBUTE *attributes;
	    CK_ULONG num_attributes;
	    int next_object;
	} find;

	int encrypt_object;
	CK_MECHANISM_PTR encrypt_mechanism;
	int decrypt_object;
	CK_MECHANISM_PTR decrypt_mechanism;
	int sign_object;
	CK_MECHANISM_PTR sign_mechanism;
	int verify_object;
	CK_MECHANISM_PTR verify_mechanism;
	int digest_object;
    } state[10];
#define MAX_NUM_SESSION (sizeof(soft_token.state)/sizeof(soft_token.state[0]))
    FILE *logfile;
} soft_token;

static void
application_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (soft_token.flags.app_error_fatal)
	abort();
}

static void
st_logf(const char *fmt, ...)
{
    if (soft_token.logfile == NULL) return;
    va_list ap;
    va_start(ap, fmt);
//     fprintf(soft_token.logfile, "                                   ** ");
    vfprintf(soft_token.logfile, fmt, ap);
    va_end(ap);
    fflush(soft_token.logfile);
}

static void
snprintf_fill(char *str, size_t size, char fillchar, const char *fmt, ...)
{
    int len;
    va_list ap;
    len = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    if (len < 0 || len > size)
	return;
    while(len < size)
	str[len++] = fillchar;
}

#ifndef TEST_APP
#define printf error_use_st_logf
#endif

#define VERIFY_SESSION_HANDLE(s, state)			\
{							\
    CK_RV ret;						\
    ret = verify_session_handle(s, state);		\
    if (ret != CKR_OK) {				\
	/* return CKR_OK */;				\
    }							\
}

static CK_RV
verify_session_handle(CK_SESSION_HANDLE hSession,
		      struct session_state **state)
{
    int i;

    for (i = 0; i < MAX_NUM_SESSION; i++){
	if (soft_token.state[i].session_handle == hSession)
	    break;
    }
    if (i == MAX_NUM_SESSION) {
	application_error("use of invalid handle: 0x%08lx\n",
			  (unsigned long)hSession);
	return CKR_SESSION_HANDLE_INVALID;
    }
    if (state)
	*state = &soft_token.state[i];
    return CKR_OK;
}

static CK_RV
object_handle_to_object(CK_OBJECT_HANDLE handle,
			struct st_object **object)
{
    int i = HANDLE_OBJECT_ID(handle);

    st_logf("object_handle_to_object\n");
    
    *object = NULL;
    if (i >= soft_token.object.num_objs)
	return CKR_ARGUMENTS_BAD;
    if (soft_token.object.objs[i] == NULL)
	return CKR_ARGUMENTS_BAD;
    if (soft_token.object.objs[i]->object_handle != handle)
	return CKR_ARGUMENTS_BAD;
    
    st_logf("object_handle_to_object COPY\n");
    *object = soft_token.object.objs[i];
    return CKR_OK;
}

static int
attributes_match(const struct st_object *obj,
		 const CK_ATTRIBUTE *attributes,
		 CK_ULONG num_attributes)
{
    CK_ULONG i;
    int j;
    st_logf("attributes_match: %ld\n", (unsigned long)OBJECT_ID(obj));

    for (i = 0; i < num_attributes; i++) {
	int match = 0;
	for (j = 0; j < obj->num_attributes; j++) {
	    if (attributes[i].type == obj->attrs[j].attribute.type &&
		attributes[i].ulValueLen == obj->attrs[j].attribute.ulValueLen &&
		memcmp(attributes[i].pValue, obj->attrs[j].attribute.pValue,
		       attributes[i].ulValueLen) == 0) {
		match = 1;
		break;
	    }
	}
	if (match == 0) {
	    st_logf("type %d attribute have no match\n", attributes[i].type);
	    return 0;
	}
    }
    st_logf("attribute matches\n");
    return 1;
}

static void
print_attributes(const CK_ATTRIBUTE *attributes,
		 CK_ULONG num_attributes)
{
    CK_ULONG i;

    st_logf("Print attributes: %lu\n", num_attributes);

    for (i = 0; i < num_attributes; i++) {
        switch (attributes[i].type) {
        case CKA_TOKEN: {
            if (attributes[i].ulValueLen != sizeof(CK_BBOOL)) {
                st_logf("  * token attribute(d) wrong length\n", CKA_TOKEN);
                break;
            }
            st_logf("  * type: <token> size: <%d> value: <%s>\n", attributes[i].ulValueLen, *((CK_BBOOL*)attributes[i].pValue) ? "TRUE" : "FALSE");
            break;
        }
        case CKA_CLASS: {
            
            if (attributes[i].ulValueLen != sizeof(CK_ULONG)) {
                st_logf("  * token attribute(%d) wrong length\n", CKA_CLASS);
                break;
            }
            CK_OBJECT_CLASS klass = *((CK_OBJECT_CLASS*)attributes[i].pValue);
            switch (klass) {
            case CKO_CERTIFICATE:
                st_logf("  * type: <class> size: <%d> value: <%s>\n", attributes[i].ulValueLen, "certificate");
                break;
            case CKO_PUBLIC_KEY:
                st_logf("  * type: <class> size: <%d> value: <%s>\n", attributes[i].ulValueLen, "public key");
                break;
            case CKO_PRIVATE_KEY:
                st_logf("  * type: <class> size: <%d> value: <%s>\n", attributes[i].ulValueLen, "private key");
                break;
            case CKO_SECRET_KEY:
                st_logf("  * type: <class> size: <%d> value: <%s>\n", attributes[i].ulValueLen, "secret key");
                break;
            case CKO_DOMAIN_PARAMETERS:
                st_logf("  * type: <class> size: <%d> value: <%s>\n", attributes[i].ulValueLen, "domain parameters");
                break;
            default:
                st_logf("  * type: <class> size: <%d> value: [class 0x%08lx]\n", attributes[i].ulValueLen, klass);
                break;
            }
            break;
        }
        case CKA_PRIVATE:
            st_logf("  * type: <private> size: <%d>\n", attributes[i].ulValueLen);
            break;
        case CKA_LABEL:
            st_logf("  * type: <label> size: <%d> value: <%s>\n", attributes[i].ulValueLen, attributes[i].pValue);
            break;
        case CKA_APPLICATION:
            st_logf("  * type: <application> size: <%d>\n", attributes[i].ulValueLen);
            break;
        case CKA_VALUE:
            st_logf("  * type: <value> size: <%d>\n", attributes[i].ulValueLen);
            break;
        case CKA_ID:
            st_logf("  * type: <id> size: <%d>\n", attributes[i].ulValueLen);
            break;
        default:
            st_logf("  * type: <UNKNOWN> size: <%d> type: [0x%08lx]\n", attributes[i].ulValueLen, attributes[i].type);
            break;
        }
    }
}

static struct st_object *
add_st_object(void)
{
    struct st_object *o, **objs;
    int i;

    o = malloc(sizeof(*o));
    if (o == NULL)
	return NULL;
    memset(o, 0, sizeof(*o));
    o->attrs = NULL;
    o->num_attributes = 0;
    
    for (i = 0; i < soft_token.object.num_objs; i++) {
	if (soft_token.object.objs == NULL) {
	    soft_token.object.objs[i] = o;
	    break;
	}
    }
    if (i == soft_token.object.num_objs) {
	objs = realloc(soft_token.object.objs,
		       (soft_token.object.num_objs + 1) * sizeof(soft_token.object.objs[0]));
	if (objs == NULL) {
	    free(o);
	    return NULL;
	}
	soft_token.object.objs = objs;
	soft_token.object.objs[soft_token.object.num_objs++] = o;
    }	
    soft_token.object.objs[i]->object_handle =
	(random() & (~OBJECT_ID_MASK)) | i;

    return o;
}

static CK_RV
add_object_attribute(struct st_object *o, 
		     int secret,
		     CK_ATTRIBUTE_TYPE type,
		     CK_VOID_PTR pValue,
		     CK_ULONG ulValueLen)
{
    struct st_attr *a;
    int i;

    i = o->num_attributes;
    a = realloc(o->attrs, (i + 1) * sizeof(o->attrs[0]));
    if (a == NULL)
	return CKR_DEVICE_MEMORY;
    o->attrs = a;
    o->attrs[i].secret = secret;
    o->attrs[i].attribute.type = type;
    o->attrs[i].attribute.pValue = malloc(ulValueLen);
    if (o->attrs[i].attribute.pValue == NULL && ulValueLen != 0)
	return CKR_DEVICE_MEMORY;
    memcpy(o->attrs[i].attribute.pValue, pValue, ulValueLen);
    o->attrs[i].attribute.ulValueLen = ulValueLen;
    o->num_attributes++;

    return CKR_OK;
}

static CK_RV
add_pubkey_info(struct st_object *o, CK_KEY_TYPE key_type, EVP_PKEY *key)
{
    switch (key_type) {
    case CKK_RSA: {
	CK_BYTE *modulus = NULL;
	size_t modulus_len = 0;
	CK_ULONG modulus_bits = 0;
	CK_BYTE *exponent = NULL;
	size_t exponent_len = 0;

	modulus_bits = BN_num_bits(key->pkey.rsa->n);

	modulus_len = BN_num_bytes(key->pkey.rsa->n);
	modulus = malloc(modulus_len);
	BN_bn2bin(key->pkey.rsa->n, modulus);
	
	exponent_len = BN_num_bytes(key->pkey.rsa->e);
	exponent = malloc(exponent_len);
	BN_bn2bin(key->pkey.rsa->e, exponent);
	
	add_object_attribute(o, 0, CKA_MODULUS, modulus, modulus_len);
	add_object_attribute(o, 0, CKA_MODULUS_BITS,
			     &modulus_bits, sizeof(modulus_bits));
	add_object_attribute(o, 0, CKA_PUBLIC_EXPONENT,
			     exponent, exponent_len);

	RSA_set_method(key->pkey.rsa, RSA_PKCS1_SSLeay());

	free(modulus);
	free(exponent);
    }
    default:
	/* XXX */
	break;
    }
    return CKR_OK;
}


static int
pem_callback(char *buf, int num, int w, void *key)
{
    return -1;
}


static CK_RV
add_certificate(char *label,
		const char *cert_file,
		const char *private_key_file,
		char *id,
		int anchor)
{
    struct st_object *o = NULL;
    CK_BBOOL bool_true = CK_TRUE;
    CK_BBOOL bool_false = CK_FALSE;
    CK_OBJECT_CLASS c;
    CK_CERTIFICATE_TYPE cert_type = CKC_X_509;
    CK_KEY_TYPE key_type;
    CK_MECHANISM_TYPE mech_type;
    void *cert_data = NULL;
    size_t cert_length;
    void *subject_data = NULL;
    size_t subject_length;
    void *issuer_data = NULL;
    size_t issuer_length;
    void *serial_data = NULL;
    size_t serial_length;
    CK_RV ret = CKR_GENERAL_ERROR;
    X509 *cert;
    EVP_PKEY *public_key;

    size_t id_len = strlen(id);

    {
	FILE *f;
	
	f = fopen(cert_file, "r");
	if (f == NULL) {
	    st_logf("failed to open file %s\n", cert_file);
	    return CKR_GENERAL_ERROR;
	}

	cert = PEM_read_X509(f, NULL, NULL, NULL);
	fclose(f);
	if (cert == NULL) {
	    st_logf("failed reading PEM cert\n");
	    return CKR_GENERAL_ERROR;
	}

	OPENSSL_ASN1_MALLOC_ENCODE(X509, cert_data, cert_length, cert, ret);
	if (ret)
	    goto out;

	OPENSSL_ASN1_MALLOC_ENCODE(X509_NAME, issuer_data, issuer_length, 
				   X509_get_issuer_name(cert), ret);
	if (ret)
	    goto out;

	OPENSSL_ASN1_MALLOC_ENCODE(X509_NAME, subject_data, subject_length, 
				   X509_get_subject_name(cert), ret);
	if (ret)
	    goto out;

	OPENSSL_ASN1_MALLOC_ENCODE(ASN1_INTEGER, serial_data, serial_length, 
				   X509_get_serialNumber(cert), ret);
	if (ret)
	    goto out;

    }

    st_logf("done parsing, adding to internal structure\n");

    o = add_st_object();
    if (o == NULL) {
	ret = CKR_DEVICE_MEMORY;
	goto out;
    }
    o->type = STO_T_CERTIFICATE;
    o->u.cert = cert;
    public_key = X509_get_pubkey(o->u.cert);

    switch (EVP_PKEY_type(public_key->type)) {
    case EVP_PKEY_RSA:
	key_type = CKK_RSA;
	break;
    case EVP_PKEY_DSA:
	key_type = CKK_DSA;
	break;
    default:
	/* XXX */
	break;
    }

    c = CKO_CERTIFICATE;
    add_object_attribute(o, 0, CKA_CLASS, &c, sizeof(c));
    add_object_attribute(o, 0, CKA_TOKEN, &bool_true, sizeof(bool_true));
    add_object_attribute(o, 0, CKA_PRIVATE, &bool_false, sizeof(bool_false));
    add_object_attribute(o, 0, CKA_MODIFIABLE, &bool_false, sizeof(bool_false));
    add_object_attribute(o, 0, CKA_LABEL, label, strlen(label));

    add_object_attribute(o, 0, CKA_CERTIFICATE_TYPE, &cert_type, sizeof(cert_type));
    add_object_attribute(o, 0, CKA_ID, id, id_len);

    add_object_attribute(o, 0, CKA_SUBJECT, subject_data, subject_length);
    add_object_attribute(o, 0, CKA_ISSUER, issuer_data, issuer_length);
    add_object_attribute(o, 0, CKA_SERIAL_NUMBER, serial_data, serial_length);
    add_object_attribute(o, 0, CKA_VALUE, cert_data, cert_length);
    if (anchor)
	add_object_attribute(o, 0, CKA_TRUSTED, &bool_true, sizeof(bool_true));
    else
	add_object_attribute(o, 0, CKA_TRUSTED, &bool_false, sizeof(bool_false));

    st_logf("add cert ok: %lx\n", (unsigned long)OBJECT_ID(o));

    o = add_st_object();
    if (o == NULL) {
	ret = CKR_DEVICE_MEMORY;
	goto out;
    }
    o->type = STO_T_PUBLIC_KEY;
    o->u.public_key = public_key;

    c = CKO_PUBLIC_KEY;
    add_object_attribute(o, 0, CKA_CLASS, &c, sizeof(c));
    add_object_attribute(o, 0, CKA_TOKEN, &bool_true, sizeof(bool_true));
    add_object_attribute(o, 0, CKA_PRIVATE, &bool_false, sizeof(bool_false));
    add_object_attribute(o, 0, CKA_MODIFIABLE, &bool_false, sizeof(bool_false));
    add_object_attribute(o, 0, CKA_LABEL, label, strlen(label));

    add_object_attribute(o, 0, CKA_KEY_TYPE, &key_type, sizeof(key_type));
    add_object_attribute(o, 0, CKA_ID, id, id_len);
    add_object_attribute(o, 0, CKA_START_DATE, "", 1); /* XXX */
    add_object_attribute(o, 0, CKA_END_DATE, "", 1); /* XXX */
    add_object_attribute(o, 0, CKA_DERIVE, &bool_false, sizeof(bool_false));
    add_object_attribute(o, 0, CKA_LOCAL, &bool_false, sizeof(bool_false));
    mech_type = CKM_RSA_X_509;
    add_object_attribute(o, 0, CKA_KEY_GEN_MECHANISM, &mech_type, sizeof(mech_type));

    add_object_attribute(o, 0, CKA_SUBJECT, subject_data, subject_length);
    add_object_attribute(o, 0, CKA_ENCRYPT, &bool_true, sizeof(bool_true));
    add_object_attribute(o, 0, CKA_VERIFY, &bool_true, sizeof(bool_true));
    add_object_attribute(o, 0, CKA_VERIFY_RECOVER, &bool_false, sizeof(bool_false));
    add_object_attribute(o, 0, CKA_WRAP, &bool_true, sizeof(bool_true));
    add_object_attribute(o, 0, CKA_TRUSTED, &bool_true, sizeof(bool_true));

    add_pubkey_info(o, key_type, public_key);

    st_logf("add key ok: %lx\n", (unsigned long)OBJECT_ID(o));

    if (private_key_file) {
	CK_FLAGS flags;
	FILE *f;

	o = add_st_object();
	if (o == NULL) {
	    ret = CKR_DEVICE_MEMORY;
	    goto out;
	}
	o->type = STO_T_PRIVATE_KEY;
	o->u.private_key.file = strdup(private_key_file);
	o->u.private_key.key = NULL;

	o->u.private_key.cert = cert;

	c = CKO_PRIVATE_KEY;
	add_object_attribute(o, 0, CKA_CLASS, &c, sizeof(c));
	add_object_attribute(o, 0, CKA_TOKEN, &bool_true, sizeof(bool_true));
	add_object_attribute(o, 0, CKA_PRIVATE, &bool_true, sizeof(bool_false));
	add_object_attribute(o, 0, CKA_MODIFIABLE, &bool_false, sizeof(bool_false));
	add_object_attribute(o, 0, CKA_LABEL, label, strlen(label));

	add_object_attribute(o, 0, CKA_KEY_TYPE, &key_type, sizeof(key_type));
	add_object_attribute(o, 0, CKA_ID, id, id_len);
	add_object_attribute(o, 0, CKA_START_DATE, "", 1); /* XXX */
	add_object_attribute(o, 0, CKA_END_DATE, "", 1); /* XXX */
	add_object_attribute(o, 0, CKA_DERIVE, &bool_false, sizeof(bool_false));
	add_object_attribute(o, 0, CKA_LOCAL, &bool_false, sizeof(bool_false));
	mech_type = CKM_RSA_X_509;
	add_object_attribute(o, 0, CKA_KEY_GEN_MECHANISM, &mech_type, sizeof(mech_type));

	add_object_attribute(o, 0, CKA_SUBJECT, subject_data, subject_length);
	add_object_attribute(o, 0, CKA_SENSITIVE, &bool_true, sizeof(bool_true));
	add_object_attribute(o, 0, CKA_SECONDARY_AUTH, &bool_false, sizeof(bool_true));
	flags = 0;
	add_object_attribute(o, 0, CKA_AUTH_PIN_FLAGS, &flags, sizeof(flags));

	add_object_attribute(o, 0, CKA_DECRYPT, &bool_true, sizeof(bool_true));
	add_object_attribute(o, 0, CKA_SIGN, &bool_true, sizeof(bool_true));
	add_object_attribute(o, 0, CKA_SIGN_RECOVER, &bool_false, sizeof(bool_false));
	add_object_attribute(o, 0, CKA_UNWRAP, &bool_true, sizeof(bool_true));
	add_object_attribute(o, 0, CKA_EXTRACTABLE, &bool_true, sizeof(bool_true));
	add_object_attribute(o, 0, CKA_NEVER_EXTRACTABLE, &bool_false, sizeof(bool_false));

	add_pubkey_info(o, key_type, public_key);

	f = fopen(private_key_file, "r");
	if (f == NULL) {
	    st_logf("failed to open private key\n");
	    return CKR_GENERAL_ERROR;
	}

	o->u.private_key.key = PEM_read_PrivateKey(f, NULL, pem_callback, NULL);
	fclose(f);
	if (o->u.private_key.key == NULL) {
	    st_logf("failed to read private key a startup\n");
	    /* don't bother with this failure for now, 
	       fix it at C_Login time */;
	} else {
	    /* XXX verify keytype */

	    if (key_type == CKK_RSA)
		RSA_set_method(o->u.private_key.key->pkey.rsa, 
			       RSA_PKCS1_SSLeay());

	    if (X509_check_private_key(cert, o->u.private_key.key) != 1) {
		EVP_PKEY_free(o->u.private_key.key);	    
		o->u.private_key.key = NULL;
		st_logf("private key doesn't verify\n");
	    } else {
		st_logf("private key usable\n");
		soft_token.flags.login_done = 1;
	    }
	}
    }

    ret = CKR_OK;
 out:
    if (ret != CKR_OK) {
	st_logf("something went wrong when adding cert!\n");

	/* XXX wack o */;
    }
    free(cert_data);
    free(serial_data);
    free(issuer_data);
    free(subject_data);

    return ret;
}

static void
find_object_final(struct session_state *state)
{
    if (state->find.attributes) {
	CK_ULONG i;

	for (i = 0; i < state->find.num_attributes; i++) {
	    if (state->find.attributes[i].pValue)
		free(state->find.attributes[i].pValue);
	}
	free(state->find.attributes);
	state->find.attributes = NULL;
	state->find.num_attributes = 0;
	state->find.next_object = -1;
    }
}

static void
reset_crypto_state(struct session_state *state)
{
    state->encrypt_object = -1;
    if (state->encrypt_mechanism)
	free(state->encrypt_mechanism);
    state->encrypt_mechanism = NULL_PTR;
    state->decrypt_object = -1;
    if (state->decrypt_mechanism)
	free(state->decrypt_mechanism);
    state->decrypt_mechanism = NULL_PTR;
    state->sign_object = -1;
    if (state->sign_mechanism)
	free(state->sign_mechanism);
    state->sign_mechanism = NULL_PTR;
    state->verify_object = -1;
    if (state->verify_mechanism)
	free(state->verify_mechanism);
    state->verify_mechanism = NULL_PTR;
    state->digest_object = -1;
}

static void
close_session(struct session_state *state)
{
    if (state->find.attributes) {
	application_error("application didn't do C_FindObjectsFinal\n");
	find_object_final(state);
    }

    state->session_handle = CK_INVALID_HANDLE;
    soft_token.application = NULL_PTR;
    soft_token.notify = NULL_PTR;
    reset_crypto_state(state);
}

static const char *
has_session(void)
{
    return soft_token.open_sessions > 0 ? "yes" : "no";
}

static void
read_conf_file(const char *fn)
{
    char buf[1024], *cert, *key, *id, *label, *s, *p;
    int anchor;
    FILE *f;

    f = fopen(fn, "r");
    if (f == NULL) {
	st_logf("can't open configuration file %s\n", fn);
	return;
    }

    while(fgets(buf, sizeof(buf), f) != NULL) {
	buf[strcspn(buf, "\n")] = '\0';

	anchor = 0;

	st_logf("line: %s\n", buf);

	p = buf;
	while (isspace(*p))
	    p++;
	if (*p == '#')
	    continue;
	while (isspace(*p))
	    p++;

	s = NULL;
	id = strtok_r(p, "\t", &s);
	if (id == NULL)
	    continue;
	label = strtok_r(NULL, "\t", &s);
	if (label == NULL)
	    continue;
	cert = strtok_r(NULL, "\t", &s);
	if (cert == NULL)
	    continue;
	key = strtok_r(NULL, "\t", &s);

	/* XXX */
	if (strcmp(id, "anchor") == 0) {
	    id = "\x00\x00";
	    anchor = 1;
	}

	st_logf("adding: %s\n", label);

	add_certificate(label, cert, key, id, anchor);
    }
}

static CK_RV
func_not_supported(void)
{
    st_logf("function not supported\n");
    return CKR_FUNCTION_NOT_SUPPORTED;
}

CK_RV
C_Initialize(CK_VOID_PTR a)
{
    CK_C_INITIALIZE_ARGS_PTR args = a;
    st_logf("Initialize\n");
    int i;

    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();

    srandom(getpid() ^ time(NULL));

    for (i = 0; i < MAX_NUM_SESSION; i++) {
	soft_token.state[i].session_handle = CK_INVALID_HANDLE;
	soft_token.state[i].find.attributes = NULL;
	soft_token.state[i].find.num_attributes = 0;
	soft_token.state[i].find.next_object = -1;
	reset_crypto_state(&soft_token.state[i]);
    }

    soft_token.flags.hardware_slot = 1;
    soft_token.flags.app_error_fatal = 0;
    soft_token.flags.login_done = 0;

    soft_token.object.objs = NULL;
    soft_token.object.num_objs = 0;
    
    soft_token.logfile = NULL;
#if 1
//     soft_token.logfile = stdout;
#endif
#if 0
    soft_token.logfile = fopen("/tmp/log-pkcs11.txt", "a");
#endif

    if (a != NULL_PTR) {
	st_logf("\tCreateMutex:\t%p\n", args->CreateMutex);
	st_logf("\tDestroyMutext\t%p\n", args->DestroyMutex);
	st_logf("\tLockMutext\t%p\n", args->LockMutex);
	st_logf("\tUnlockMutext\t%p\n", args->UnlockMutex);
	st_logf("\tFlags\t%04x\n", (unsigned int)args->flags);
    }

    {
	char *fn = NULL, *home = NULL;

	if (getuid() == geteuid()) {
	    fn = getenv("SOFTPKCS11RC");
	    if (fn)
		fn = strdup(fn);
	    home = getenv("HOME");
	}
	if (fn == NULL && home == NULL) {
	    struct passwd *pw = getpwuid(getuid());	
	    if(pw != NULL)
		home = pw->pw_dir;
	}
	if (fn == NULL) {
	    if (home)
		asprintf(&fn, "%s/.soft-token.rc", home);
	    else
		fn = strdup("/etc/soft-token.rc");
	}

	read_conf_file(fn);
	free(fn);
    }

    return CKR_OK;
}

CK_RV
C_Finalize(CK_VOID_PTR args)
{
    int i;

    st_logf("Finalize\n");

    for (i = 0; i < MAX_NUM_SESSION; i++) {
	if (soft_token.state[i].session_handle != CK_INVALID_HANDLE) {
	    application_error("application finalized without "
			      "closing session\n");
	    close_session(&soft_token.state[i]);
	}
    }

    return CKR_OK;
}

CK_RV
C_GetInfo(CK_INFO_PTR args)
{
    st_logf("GetInfo\n");

    memset(args, 17, sizeof(*args));
    args->cryptokiVersion.major = 2;
    args->cryptokiVersion.minor = 10;
    snprintf_fill((char *)args->manufacturerID, 
		  sizeof(args->manufacturerID),
		  ' ',
		  "SoftToken");
    snprintf_fill((char *)args->libraryDescription, 
		  sizeof(args->libraryDescription), ' ',
		  "SoftToken");
    args->libraryVersion.major = 1;
    args->libraryVersion.minor = 8;

    return CKR_OK;
}

extern CK_FUNCTION_LIST funcs;

CK_RV
C_GetFunctionList(CK_FUNCTION_LIST_PTR_PTR ppFunctionList)
{
    *ppFunctionList = &funcs;
    return CKR_OK;
}

CK_RV
C_GetSlotList(CK_BBOOL tokenPresent,
	      CK_SLOT_ID_PTR pSlotList,
	      CK_ULONG_PTR   pulCount)
{
    st_logf("GetSlotList: %s\n",
	    tokenPresent ? "tokenPresent" : "token not Present");
    if (pSlotList)
	pSlotList[0] = 1;
    *pulCount = 1;
    return CKR_OK;
}

CK_RV
C_GetSlotInfo(CK_SLOT_ID slotID,
	      CK_SLOT_INFO_PTR pInfo)
{
    st_logf("GetSlotInfo: slot: %d : %s\n", (int)slotID, has_session());

    memset(pInfo, 18, sizeof(*pInfo));

    if (slotID != 1)
	return CKR_ARGUMENTS_BAD;

    snprintf_fill((char *)pInfo->slotDescription, 
		  sizeof(pInfo->slotDescription),
		  ' ',
		  "SoftToken (slot)");
    snprintf_fill((char *)pInfo->manufacturerID,
		  sizeof(pInfo->manufacturerID),
		  ' ',
		  "SoftToken (slot)");
    pInfo->flags = CKF_TOKEN_PRESENT;
    if (soft_token.flags.hardware_slot)
	pInfo->flags |= CKF_HW_SLOT;
    pInfo->hardwareVersion.major = 1;
    pInfo->hardwareVersion.minor = 0;
    pInfo->firmwareVersion.major = 1;
    pInfo->firmwareVersion.minor = 0;
    
    return CKR_OK;
}

CK_RV
C_GetTokenInfo(CK_SLOT_ID slotID,
	       CK_TOKEN_INFO_PTR pInfo)
{
    st_logf("GetTokenInfo: %s\n", has_session()); 

    memset(pInfo, 19, sizeof(*pInfo));

    snprintf_fill((char *)pInfo->label, 
		  sizeof(pInfo->label),
		  ' ',
		  "SoftToken (token)");
    snprintf_fill((char *)pInfo->manufacturerID, 
		  sizeof(pInfo->manufacturerID),
		  ' ',
		  "SoftToken (token)");
    snprintf_fill((char *)pInfo->model,
		  sizeof(pInfo->model),
		  ' ',
		  "SoftToken (token)");
    snprintf_fill((char *)pInfo->serialNumber, 
		  sizeof(pInfo->serialNumber),
		  ' ',
		  "4711");
    pInfo->flags = 
	CKF_TOKEN_INITIALIZED | 
	CKF_USER_PIN_INITIALIZED;

    if (soft_token.flags.login_done == 0)
	pInfo->flags |= CKF_LOGIN_REQUIRED;

    /* CFK_RNG |
       CKF_RESTORE_KEY_NOT_NEEDED |
    */
    pInfo->ulMaxSessionCount = MAX_NUM_SESSION;
    pInfo->ulSessionCount = soft_token.open_sessions;
    pInfo->ulMaxRwSessionCount = MAX_NUM_SESSION;
    pInfo->ulRwSessionCount = soft_token.open_sessions;
    pInfo->ulMaxPinLen = 1024;
    pInfo->ulMinPinLen = 0;
    pInfo->ulTotalPublicMemory = 4711;
    pInfo->ulFreePublicMemory = 4712;
    pInfo->ulTotalPrivateMemory = 4713;
    pInfo->ulFreePrivateMemory = 4714;
    pInfo->hardwareVersion.major = 2;
    pInfo->hardwareVersion.minor = 0;
    pInfo->firmwareVersion.major = 2;
    pInfo->firmwareVersion.minor = 0;

    return CKR_OK;
}

CK_RV
C_GetMechanismList(CK_SLOT_ID slotID,
		   CK_MECHANISM_TYPE_PTR pMechanismList,
		   CK_ULONG_PTR pulCount)
{
    st_logf("GetMechanismList\n");

    *pulCount = 2;
    if (pMechanismList == NULL_PTR)
	return CKR_OK;
    pMechanismList[0] = CKM_RSA_X_509;
    pMechanismList[1] = CKM_RSA_PKCS;

    return CKR_OK;
}

CK_RV
C_GetMechanismInfo(CK_SLOT_ID slotID,
		   CK_MECHANISM_TYPE type,
		   CK_MECHANISM_INFO_PTR pInfo)
{
    st_logf("GetMechanismInfo: slot %d type: %d\n",
	    (int)slotID, (int)type);
    return CKR_FUNCTION_NOT_SUPPORTED;
}

CK_RV
C_InitToken(CK_SLOT_ID slotID,
	    CK_UTF8CHAR_PTR pPin,
	    CK_ULONG ulPinLen,
	    CK_UTF8CHAR_PTR pLabel)
{
    st_logf("InitToken: slot %d\n", (int)slotID);
    return CKR_FUNCTION_NOT_SUPPORTED;
}

CK_RV
C_OpenSession(CK_SLOT_ID slotID,
	      CK_FLAGS flags,
	      CK_VOID_PTR pApplication,
	      CK_NOTIFY Notify,
	      CK_SESSION_HANDLE_PTR phSession)
{
    int i;

    st_logf("OpenSession: slot: %d\n", (int)slotID);
    
    if (soft_token.open_sessions == MAX_NUM_SESSION)
	return CKR_SESSION_COUNT;

    soft_token.application = pApplication;
    soft_token.notify = Notify;

    for (i = 0; i < MAX_NUM_SESSION; i++)
	if (soft_token.state[i].session_handle == CK_INVALID_HANDLE)
	    break;
    if (i == MAX_NUM_SESSION)
	abort();

    soft_token.open_sessions++;

    soft_token.state[i].session_handle =
	(CK_SESSION_HANDLE)(random() & 0xfffff);
    *phSession = soft_token.state[i].session_handle;
    st_logf("  == OpenSession: Session: %d\n", phSession);

    return CKR_OK;
}

CK_RV
C_CloseSession(CK_SESSION_HANDLE hSession)
{
    struct session_state *state;
    st_logf("CloseSession\n");

    if (verify_session_handle(hSession, &state) != CKR_OK)
	application_error("closed session not open");
    else
	close_session(state);

    return CKR_OK;
}

CK_RV
C_CloseAllSessions(CK_SLOT_ID slotID)
{
    int i;

    st_logf("CloseAllSessions\n");

    for (i = 0; i < MAX_NUM_SESSION; i++)
	if (soft_token.state[i].session_handle != CK_INVALID_HANDLE)
	    close_session(&soft_token.state[i]);

    return CKR_OK;
}

CK_RV
C_GetSessionInfo(CK_SESSION_HANDLE hSession,
		 CK_SESSION_INFO_PTR pInfo)
{
    st_logf("GetSessionInfo\n");
    
    VERIFY_SESSION_HANDLE(hSession, NULL);

    memset(pInfo, 20, sizeof(*pInfo));

    pInfo->slotID = 1;
    if (soft_token.flags.login_done)
	pInfo->state = CKS_RO_USER_FUNCTIONS;
    else
	pInfo->state = CKS_RO_PUBLIC_SESSION;
    pInfo->flags = CKF_SERIAL_SESSION;
    pInfo->ulDeviceError = 0;

    return CKR_OK;
}

CK_RV
C_Login(CK_SESSION_HANDLE hSession,
	CK_USER_TYPE userType,
	CK_UTF8CHAR_PTR pPin,
	CK_ULONG ulPinLen)
{
    char *pin = NULL;
    int i;

    st_logf("Login\n");

    VERIFY_SESSION_HANDLE(hSession, NULL);

    if (pPin != NULL_PTR) {
	asprintf(&pin, "%.*s", (int)ulPinLen, pPin);
	st_logf("type: %d password: %s\n", (int)userType, pin);
    }

    for (i = 0; i < soft_token.object.num_objs; i++) {
	struct st_object *o = soft_token.object.objs[i];
	FILE *f;

	if (o->type != STO_T_PRIVATE_KEY)
	    continue;

	if (o->u.private_key.key)
	    continue;

	f = fopen(o->u.private_key.file, "r");
	if (f == NULL) {
	    st_logf("can't open private file: %s\n", o->u.private_key.file);
	    continue;
	}
	
	o->u.private_key.key = PEM_read_PrivateKey(f, NULL, NULL, pin);
	fclose(f);
	if (o->u.private_key.key == NULL) {
	    st_logf("failed to read key: %s error: %s\n",
		    o->u.private_key.file, 
		    ERR_error_string(ERR_get_error(), NULL));
	    /* just ignore failure */;
	    continue;
	}

	/* XXX check keytype */
	RSA_set_method(o->u.private_key.key->pkey.rsa, RSA_PKCS1_SSLeay());

	if (X509_check_private_key(o->u.private_key.cert, o->u.private_key.key) != 1) {
	    EVP_PKEY_free(o->u.private_key.key);	    
	    o->u.private_key.key = NULL;
	    st_logf("private key %s doesn't verify\n", o->u.private_key.file);
	    continue;
	}

	soft_token.flags.login_done = 1;
    }
    free(pin);
    
    return soft_token.flags.login_done ? CKR_OK : CKR_PIN_INCORRECT;
}

CK_RV
C_Logout(CK_SESSION_HANDLE hSession)
{
    st_logf("Logout\n");
    VERIFY_SESSION_HANDLE(hSession, NULL);
    return CKR_FUNCTION_NOT_SUPPORTED;
}

CK_RV
C_GetObjectSize(CK_SESSION_HANDLE hSession,
		CK_OBJECT_HANDLE hObject,
		CK_ULONG_PTR pulSize)
{
    st_logf("GetObjectSize\n");
    VERIFY_SESSION_HANDLE(hSession, NULL);
    return CKR_FUNCTION_NOT_SUPPORTED;
}

CK_RV
C_GetAttributeValue(CK_SESSION_HANDLE hSession,
		    CK_OBJECT_HANDLE hObject,
		    CK_ATTRIBUTE_PTR pTemplate,
		    CK_ULONG ulCount)
{
    struct session_state *state;
    struct st_object *obj;
    CK_ULONG i;
    CK_RV ret;
    int j;

   
    st_logf("GetAttributeValue: %lx count: %d\n",
	    (unsigned long)HANDLE_OBJECT_ID(hObject), ulCount);
    
    st_logf(" input ");
    print_attributes(pTemplate, ulCount);
    
    if ((unsigned long)HANDLE_OBJECT_ID(hObject) != 2) return CKR_ARGUMENTS_BAD;
    
    VERIFY_SESSION_HANDLE(hSession, &state);

    
    
    if ((ret = object_handle_to_object(hObject, &obj)) != CKR_OK) {
        st_logf("object not found: %lx\n", (unsigned long)HANDLE_OBJECT_ID(hObject));
        return ret;
    }

    for (i = 0; i < ulCount; i++) {
        st_logf("	getting 0x%08lx\n", (unsigned long)pTemplate[i].type);
        for (j = 0; j < obj->num_attributes; j++) {
            if (obj->attrs[j].secret) {
                pTemplate[i].ulValueLen = (CK_ULONG)-1;
                st_logf("fckn SECRET\n");
                break;
            }
            if (pTemplate[i].type == obj->attrs[j].attribute.type) {
                if (pTemplate[i].type == CKA_LABEL) st_logf("     !!! LABNEL: %s\n", obj->attrs[j].attribute.pValue);                
                if (pTemplate[i].pValue != NULL_PTR && obj->attrs[j].secret == 0) {
                    st_logf("     tmpl mem size: %d\n", pTemplate[i].ulValueLen);
                    if (pTemplate[i].ulValueLen >= obj->attrs[j].attribute.ulValueLen) {
                        memcpy(pTemplate[i].pValue, obj->attrs[j].attribute.pValue,
                            obj->attrs[j].attribute.ulValueLen);
                    }
                }
                pTemplate[i].ulValueLen = obj->attrs[j].attribute.ulValueLen;
                break;
            }
        }
        if (j == obj->num_attributes) {
            st_logf("key type: 0x%08lx not found\n", (unsigned long)pTemplate[i].type);
            pTemplate[i].ulValueLen = (CK_ULONG)-1;
        }

    }
    
    st_logf(" output ");
    print_attributes(pTemplate, ulCount);
    return CKR_OK;
}

CK_RV
C_FindObjectsInit(CK_SESSION_HANDLE hSession,
		  CK_ATTRIBUTE_PTR pTemplate,
		  CK_ULONG ulCount)
{
    struct session_state *state;

    st_logf("FindObjectsInit\n");

    VERIFY_SESSION_HANDLE(hSession, &state);

    if (state->find.next_object != -1) {
        application_error("application didn't do C_FindObjectsFinal\n");
        find_object_final(state);
    }
    if (ulCount) {
        CK_ULONG i;
        size_t len;

        print_attributes(pTemplate, ulCount);

        state->find.attributes = 
            calloc(1, ulCount * sizeof(state->find.attributes[0]));
        if (state->find.attributes == NULL)
            return CKR_DEVICE_MEMORY;
        for (i = 0; i < ulCount; i++) {
            state->find.attributes[i].pValue = 
            malloc(pTemplate[i].ulValueLen);
            if (state->find.attributes[i].pValue == NULL) {
            find_object_final(state);
            return CKR_DEVICE_MEMORY;
            }
            memcpy(state->find.attributes[i].pValue,
            pTemplate[i].pValue, pTemplate[i].ulValueLen);
            state->find.attributes[i].type = pTemplate[i].type;
            state->find.attributes[i].ulValueLen = pTemplate[i].ulValueLen;
        }
        state->find.num_attributes = ulCount;
        state->find.next_object = 0;
    } else {
        st_logf("find all objects\n");
        state->find.attributes = NULL;
        state->find.num_attributes = 0;
        state->find.next_object = 0;
    }

    return CKR_OK;
}

CK_RV
C_FindObjects(CK_SESSION_HANDLE hSession, 
	      CK_OBJECT_HANDLE_PTR phObject,
	      CK_ULONG ulMaxObjectCount,
	      CK_ULONG_PTR pulObjectCount)
{
    struct session_state *state;
    int i;

    st_logf("FindObjects\n");

    VERIFY_SESSION_HANDLE(hSession, &state);

    if (state->find.next_object == -1) {
        application_error("application didn't do C_FindObjectsInit\n");
        return CKR_ARGUMENTS_BAD;
    }
    if (ulMaxObjectCount == 0) {
        application_error("application asked for 0 objects\n");
        return CKR_ARGUMENTS_BAD;
    }
    
    *pulObjectCount = 0;
    for (i = state->find.next_object; i < soft_token.object.num_objs; i++) {
        st_logf("FindObjects: %d\n", i);
        state->find.next_object = i + 1;
        if (attributes_match(soft_token.object.objs[i],
                    state->find.attributes,
                    state->find.num_attributes)) {
            *phObject++ = soft_token.object.objs[i]->object_handle;
            st_logf("  == Object: %d\n", soft_token.object.objs[i]->object_handle);
            ulMaxObjectCount--;
            (*pulObjectCount)++;
            if (ulMaxObjectCount == 0) break;
        }
    }
    
    st_logf("  == pulObjectCount: %d\n", *pulObjectCount);
    return CKR_OK;
}

CK_RV
C_FindObjectsFinal(CK_SESSION_HANDLE hSession)
{
    struct session_state *state;

    st_logf("FindObjectsFinal\n");
    VERIFY_SESSION_HANDLE(hSession, &state);
    find_object_final(state);
    return CKR_OK;
}

static CK_RV
commonInit(CK_ATTRIBUTE *attr_match, int attr_match_len,
	   const CK_MECHANISM_TYPE *mechs, int mechs_len,
	   const CK_MECHANISM_PTR pMechanism, CK_OBJECT_HANDLE hKey,
	   struct st_object **o)
{
    CK_RV ret;
    int i;

    *o = NULL;
    if ((ret = object_handle_to_object(hKey, o)) != CKR_OK)
	return ret;

    ret = attributes_match(*o, attr_match, attr_match_len);
    if (!ret) {
	application_error("called commonInit on key that doesn't "
			  "support required attr");
	return CKR_ARGUMENTS_BAD;
    }

    for (i = 0; i < mechs_len; i++)
	if (mechs[i] == pMechanism->mechanism)
	    break;
    if (i == mechs_len) {
	application_error("called mech (%08lx) not supported\n",
			  pMechanism->mechanism);
	return CKR_ARGUMENTS_BAD;
    }
    return CKR_OK;
}


static CK_RV
dup_mechanism(CK_MECHANISM_PTR *dup, const CK_MECHANISM_PTR pMechanism)
{
    CK_MECHANISM_PTR p;

    p = malloc(sizeof(*p));
    if (p == NULL)
	return CKR_DEVICE_MEMORY;

    if (*dup)
	free(*dup);
    *dup = p;
    memcpy(p, pMechanism, sizeof(*p));

    return CKR_OK;
}


CK_RV
C_EncryptInit(CK_SESSION_HANDLE hSession,
	      CK_MECHANISM_PTR pMechanism,
	      CK_OBJECT_HANDLE hKey)
{
    struct session_state *state;
    CK_MECHANISM_TYPE mechs[] = { CKM_RSA_PKCS, CKM_RSA_X_509 };
    CK_BBOOL bool_true = CK_TRUE;
    CK_ATTRIBUTE attr[] = {
	{ CKA_ENCRYPT, &bool_true, sizeof(bool_true) }
    };
    struct st_object *o;
    CK_RV ret;

    st_logf("EncryptInit\n");
    VERIFY_SESSION_HANDLE(hSession, &state);
    
    ret = commonInit(attr, sizeof(attr)/sizeof(attr[0]), 
		     mechs, sizeof(mechs)/sizeof(mechs[0]),
		     pMechanism, hKey, &o);
    if (ret)
	return ret;

    ret = dup_mechanism(&state->encrypt_mechanism, pMechanism);
    if (ret == CKR_OK) 
	state->encrypt_object = OBJECT_ID(o);
			
    return ret;
}

CK_RV
C_Encrypt(CK_SESSION_HANDLE hSession,
	  CK_BYTE_PTR pData,
	  CK_ULONG ulDataLen,
	  CK_BYTE_PTR pEncryptedData,
	  CK_ULONG_PTR pulEncryptedDataLen)
{
    struct session_state *state;
    struct st_object *o;
    void *buffer = NULL;
    CK_RV ret;
    RSA *rsa;
    int padding, len, buffer_len, padding_len;

    st_logf("Encrypt\n");

    VERIFY_SESSION_HANDLE(hSession, &state);

    if (state->encrypt_object == -1)
	return CKR_ARGUMENTS_BAD;

    o = soft_token.object.objs[state->encrypt_object];

    if (o->u.public_key == NULL) {
	st_logf("public key NULL\n");
	return CKR_ARGUMENTS_BAD;
    }

    rsa = o->u.public_key->pkey.rsa;

    if (rsa == NULL)
	return CKR_ARGUMENTS_BAD;

    RSA_blinding_off(rsa); /* XXX RAND is broken while running in mozilla ? */

    buffer_len = RSA_size(rsa);

    buffer = malloc(buffer_len);
    if (buffer == NULL) {
	ret = CKR_DEVICE_MEMORY;
	goto out;
    }

    ret = CKR_OK;
    switch(state->encrypt_mechanism->mechanism) {
    case CKM_RSA_PKCS:
	padding = RSA_PKCS1_PADDING;
	padding_len = RSA_PKCS1_PADDING_SIZE;
	break;
    case CKM_RSA_X_509:
	padding = RSA_NO_PADDING;
	padding_len = 0;
	break;
    default:
	ret = CKR_FUNCTION_NOT_SUPPORTED;
	goto out;
    }

    if (buffer_len + padding_len < ulDataLen) {
	ret = CKR_ARGUMENTS_BAD;
	goto out;
    }

    if (pulEncryptedDataLen == NULL) {
	st_logf("pulEncryptedDataLen NULL\n");
	ret = CKR_ARGUMENTS_BAD;
	goto out;
    }

    if (pData == NULL_PTR) {
	st_logf("data NULL\n");
	ret = CKR_ARGUMENTS_BAD;
	goto out;
    }

    len = RSA_public_encrypt(ulDataLen, pData, buffer, rsa, padding);
    if (len <= 0) {
	ret = CKR_DEVICE_ERROR;
	goto out;
    }
    if (len > buffer_len)
	abort();
	
    if (pEncryptedData != NULL_PTR)
	memcpy(pEncryptedData, buffer, len);
    *pulEncryptedDataLen = len;

 out:
    if (buffer) {
	memset(buffer, 0, buffer_len);
	free(buffer);
    }
    return ret;
}

CK_RV
C_EncryptUpdate(CK_SESSION_HANDLE hSession,
		CK_BYTE_PTR pPart,
		CK_ULONG ulPartLen,
		CK_BYTE_PTR pEncryptedPart,
		CK_ULONG_PTR pulEncryptedPartLen)
{
    st_logf("EncryptUpdate\n");
    VERIFY_SESSION_HANDLE(hSession, NULL);
    return CKR_FUNCTION_NOT_SUPPORTED;
}


CK_RV
C_EncryptFinal(CK_SESSION_HANDLE hSession,
	       CK_BYTE_PTR pLastEncryptedPart,
	       CK_ULONG_PTR pulLastEncryptedPartLen)
{
    st_logf("EncryptFinal\n");
    VERIFY_SESSION_HANDLE(hSession, NULL);
    return CKR_FUNCTION_NOT_SUPPORTED;
}


/* C_DecryptInit initializes a decryption operation. */
CK_RV
C_DecryptInit(CK_SESSION_HANDLE hSession,
	      CK_MECHANISM_PTR pMechanism,
	      CK_OBJECT_HANDLE hKey)
{
    struct session_state *state;
    CK_MECHANISM_TYPE mechs[] = { CKM_RSA_PKCS, CKM_RSA_X_509 };
    CK_BBOOL bool_true = CK_TRUE;
    CK_ATTRIBUTE attr[] = {
	{ CKA_DECRYPT, &bool_true, sizeof(bool_true) }
    };
    struct st_object *o;
    CK_RV ret;

    st_logf("DecryptInit\n");
    VERIFY_SESSION_HANDLE(hSession, &state);
    
    ret = commonInit(attr, sizeof(attr)/sizeof(attr[0]), 
		     mechs, sizeof(mechs)/sizeof(mechs[0]),
		     pMechanism, hKey, &o);
    if (ret)
	return ret;

    ret = dup_mechanism(&state->decrypt_mechanism, pMechanism);
    if (ret == CKR_OK) 
	state->decrypt_object = OBJECT_ID(o);

    return CKR_OK;
}


CK_RV
C_Decrypt(CK_SESSION_HANDLE hSession,
	  CK_BYTE_PTR       pEncryptedData,
	  CK_ULONG          ulEncryptedDataLen,
	  CK_BYTE_PTR       pData,
	  CK_ULONG_PTR      pulDataLen)
{
    struct session_state *state;
    struct st_object *o;
    void *buffer = NULL;
    CK_RV ret;
    RSA *rsa;
    int padding, len, buffer_len, padding_len;

    st_logf("Decrypt\n");

    VERIFY_SESSION_HANDLE(hSession, &state);

    if (state->decrypt_object == -1)
	return CKR_ARGUMENTS_BAD;

    o = soft_token.object.objs[state->decrypt_object];

    if (o->u.private_key.key == NULL) {
	st_logf("private key NULL\n");
	return CKR_ARGUMENTS_BAD;
    }

    rsa = o->u.private_key.key->pkey.rsa;

    if (rsa == NULL)
	return CKR_ARGUMENTS_BAD;

    RSA_blinding_off(rsa); /* XXX RAND is broken while running in mozilla ? */

    buffer_len = RSA_size(rsa);

    buffer = malloc(buffer_len);
    if (buffer == NULL) {
	ret = CKR_DEVICE_MEMORY;
	goto out;
    }

    ret = CKR_OK;
    switch(state->decrypt_mechanism->mechanism) {
    case CKM_RSA_PKCS:
	padding = RSA_PKCS1_PADDING;
	padding_len = RSA_PKCS1_PADDING_SIZE;
	break;
    case CKM_RSA_X_509:
	padding = RSA_NO_PADDING;
	padding_len = 0;
	break;
    default:
	ret = CKR_FUNCTION_NOT_SUPPORTED;
	goto out;
    }

    if (buffer_len + padding_len < ulEncryptedDataLen) {
	ret = CKR_ARGUMENTS_BAD;
	goto out;
    }

    if (pulDataLen == NULL) {
	st_logf("pulDataLen NULL\n");
	ret = CKR_ARGUMENTS_BAD;
	goto out;
    }

    if (pEncryptedData == NULL_PTR) {
	st_logf("data NULL\n");
	ret = CKR_ARGUMENTS_BAD;
	goto out;
    }

    len = RSA_private_decrypt(ulEncryptedDataLen, pEncryptedData, buffer, 
			      rsa, padding);
    if (len <= 0) {
	ret = CKR_DEVICE_ERROR;
	goto out;
    }
    if (len > buffer_len)
	abort();
	
    if (pData != NULL_PTR)
	memcpy(pData, buffer, len);
    *pulDataLen = len;

 out:
    if (buffer) {
	memset(buffer, 0, buffer_len);
	free(buffer);
    }
    return ret;
}


CK_RV
C_DecryptUpdate(CK_SESSION_HANDLE hSession,
		CK_BYTE_PTR pEncryptedPart,
		CK_ULONG ulEncryptedPartLen,
		CK_BYTE_PTR pPart,
		CK_ULONG_PTR pulPartLen)

{
    st_logf("DecryptUpdate\n");
    VERIFY_SESSION_HANDLE(hSession, NULL);
    return CKR_FUNCTION_NOT_SUPPORTED;
}


CK_RV
C_DecryptFinal(CK_SESSION_HANDLE hSession,
	       CK_BYTE_PTR pLastPart,
	       CK_ULONG_PTR pulLastPartLen)
{
    st_logf("DecryptFinal\n");
    VERIFY_SESSION_HANDLE(hSession, NULL);
    return CKR_FUNCTION_NOT_SUPPORTED;
}

CK_RV
C_DigestInit(CK_SESSION_HANDLE hSession,
	     CK_MECHANISM_PTR pMechanism)
{
    st_logf("DigestInit\n");
    VERIFY_SESSION_HANDLE(hSession, NULL);
    return CKR_FUNCTION_NOT_SUPPORTED;
}

CK_RV
C_SignInit(CK_SESSION_HANDLE hSession,
	   CK_MECHANISM_PTR pMechanism,
	   CK_OBJECT_HANDLE hKey)
{
    struct session_state *state;
    CK_MECHANISM_TYPE mechs[] = { CKM_RSA_PKCS, CKM_RSA_X_509 };
    CK_BBOOL bool_true = CK_TRUE;
    CK_ATTRIBUTE attr[] = {
	{ CKA_SIGN, &bool_true, sizeof(bool_true) }
    };
    struct st_object *o;
    CK_RV ret;

    st_logf("SignInit\n");
    VERIFY_SESSION_HANDLE(hSession, &state);
    
    ret = commonInit(attr, sizeof(attr)/sizeof(attr[0]), 
		     mechs, sizeof(mechs)/sizeof(mechs[0]),
		     pMechanism, hKey, &o);
    if (ret)
	return ret;

    ret = dup_mechanism(&state->sign_mechanism, pMechanism);
    if (ret == CKR_OK) 
	state->sign_object = OBJECT_ID(o);

    return CKR_OK;
}

CK_RV
C_Sign(CK_SESSION_HANDLE hSession,
       CK_BYTE_PTR pData,
       CK_ULONG ulDataLen,
       CK_BYTE_PTR pSignature,
       CK_ULONG_PTR pulSignatureLen)
{
    struct session_state *state;
    struct st_object *o;
    void *buffer = NULL;
    CK_RV ret;
    RSA *rsa;
    int padding, len, buffer_len, padding_len;

    st_logf("Sign\n");
    VERIFY_SESSION_HANDLE(hSession, &state);

    if (state->sign_object == -1)
	return CKR_ARGUMENTS_BAD;

    o = soft_token.object.objs[state->sign_object];

    if (o->u.private_key.key == NULL) {
	st_logf("private key NULL\n");
	return CKR_ARGUMENTS_BAD;
    }

    rsa = o->u.private_key.key->pkey.rsa;

    if (rsa == NULL)
	return CKR_ARGUMENTS_BAD;

    RSA_blinding_off(rsa); /* XXX RAND is broken while running in mozilla ? */

    buffer_len = RSA_size(rsa);

    buffer = malloc(buffer_len);
    if (buffer == NULL) {
	ret = CKR_DEVICE_MEMORY;
	goto out;
    }

    switch(state->sign_mechanism->mechanism) {
    case CKM_RSA_PKCS:
	padding = RSA_PKCS1_PADDING;
	padding_len = RSA_PKCS1_PADDING_SIZE;
	break;
    case CKM_RSA_X_509:
	padding = RSA_NO_PADDING;
	padding_len = 0;
	break;
    default:
	ret = CKR_FUNCTION_NOT_SUPPORTED;
	goto out;
    }

    if (buffer_len < ulDataLen + padding_len) {
	ret = CKR_ARGUMENTS_BAD;
	goto out;
    }

    if (pulSignatureLen == NULL) {
	st_logf("signature len NULL\n");
	ret = CKR_ARGUMENTS_BAD;
	goto out;
    }

    if (pData == NULL_PTR) {
	st_logf("data NULL\n");
	ret = CKR_ARGUMENTS_BAD;
	goto out;
    }

    len = RSA_private_encrypt(ulDataLen, pData, buffer, rsa, padding);
    st_logf("private encrypt done\n");
    if (len <= 0) {
	ret = CKR_DEVICE_ERROR;
	goto out;
    }
    if (len > buffer_len)
	abort();
	
    if (pSignature != NULL_PTR)
	memcpy(pSignature, buffer, len);
    *pulSignatureLen = len;

    ret = CKR_OK;

 out:
    if (buffer) {
	memset(buffer, 0, buffer_len);
	free(buffer);
    }
    return ret;
}

CK_RV
C_SignUpdate(CK_SESSION_HANDLE hSession,
	     CK_BYTE_PTR pPart,
	     CK_ULONG ulPartLen)
{
    st_logf("SignUpdate\n");
    VERIFY_SESSION_HANDLE(hSession, NULL);
    return CKR_FUNCTION_NOT_SUPPORTED;
}


CK_RV
C_SignFinal(CK_SESSION_HANDLE hSession,
	    CK_BYTE_PTR pSignature,
	    CK_ULONG_PTR pulSignatureLen)
{
    st_logf("SignUpdate\n");
    VERIFY_SESSION_HANDLE(hSession, NULL);
    return CKR_FUNCTION_NOT_SUPPORTED;
}

CK_RV
C_VerifyInit(CK_SESSION_HANDLE hSession,
	     CK_MECHANISM_PTR pMechanism,
	     CK_OBJECT_HANDLE hKey)
{
    struct session_state *state;
    CK_MECHANISM_TYPE mechs[] = { CKM_RSA_PKCS, CKM_RSA_X_509 };
    CK_BBOOL bool_true = CK_TRUE;
    CK_ATTRIBUTE attr[] = {
	{ CKA_VERIFY, &bool_true, sizeof(bool_true) }
    };
    struct st_object *o;
    CK_RV ret;

    st_logf("VerifyInit\n");
    VERIFY_SESSION_HANDLE(hSession, &state);
    
    ret = commonInit(attr, sizeof(attr)/sizeof(attr[0]), 
		     mechs, sizeof(mechs)/sizeof(mechs[0]),
		     pMechanism, hKey, &o);
    if (ret)
	return ret;

    ret = dup_mechanism(&state->verify_mechanism, pMechanism);
    if (ret == CKR_OK) 
	state->verify_object = OBJECT_ID(o);
			
    return ret;
}

CK_RV
C_Verify(CK_SESSION_HANDLE hSession,
	 CK_BYTE_PTR pData,
	 CK_ULONG ulDataLen,
	 CK_BYTE_PTR pSignature,
	 CK_ULONG ulSignatureLen)
{
    struct session_state *state;
    struct st_object *o;
    void *buffer = NULL;
    CK_RV ret;
    RSA *rsa;
    int padding, len, buffer_len;

    st_logf("Verify\n");
    VERIFY_SESSION_HANDLE(hSession, &state);

    if (state->verify_object == -1)
	return CKR_ARGUMENTS_BAD;

    o = soft_token.object.objs[state->verify_object];

    if (o->u.public_key == NULL) {
	st_logf("public key NULL\n");
	return CKR_ARGUMENTS_BAD;
    }

    rsa = o->u.public_key->pkey.rsa;

    if (rsa == NULL)
	return CKR_ARGUMENTS_BAD;

    RSA_blinding_off(rsa); /* XXX RAND is broken while running in mozilla ? */

    buffer_len = RSA_size(rsa);

    buffer = malloc(buffer_len);
    if (buffer == NULL) {
	ret = CKR_DEVICE_MEMORY;
	goto out;
    }

    ret = CKR_OK;
    switch(state->verify_mechanism->mechanism) {
    case CKM_RSA_PKCS:
	padding = RSA_PKCS1_PADDING;
	break;
    case CKM_RSA_X_509:
	padding = RSA_NO_PADDING;
	break;
    default:
	ret = CKR_FUNCTION_NOT_SUPPORTED;
	goto out;
    }

    if (buffer_len < ulDataLen) {
	ret = CKR_ARGUMENTS_BAD;
	goto out;
    }

    if (pSignature == NULL) {
	st_logf("signature NULL\n");
	ret = CKR_ARGUMENTS_BAD;
	goto out;
    }

    if (pData == NULL_PTR) {
	st_logf("data NULL\n");
	ret = CKR_ARGUMENTS_BAD;
	goto out;
    }

    len = RSA_public_decrypt(ulDataLen, pData, buffer, rsa, padding);
    st_logf("private encrypt done\n");
    if (len <= 0) {
	ret = CKR_DEVICE_ERROR;
	goto out;
    }
    if (len > buffer_len)
	abort();
	
    if (len != ulSignatureLen) {
	ret = CKR_GENERAL_ERROR;
	goto out;
    }
	
    if (memcmp(pSignature, buffer, len) != 0) {
	ret = CKR_GENERAL_ERROR;
	goto out;
    }

 out:
    if (buffer) {
	memset(buffer, 0, buffer_len);
	free(buffer);
    }
    return ret;
}


CK_RV
C_VerifyUpdate(CK_SESSION_HANDLE hSession,
	       CK_BYTE_PTR pPart,
	       CK_ULONG ulPartLen)
{
    st_logf("VerifyUpdate\n");
    VERIFY_SESSION_HANDLE(hSession, NULL);
    return CKR_FUNCTION_NOT_SUPPORTED;
}

CK_RV
C_VerifyFinal(CK_SESSION_HANDLE hSession,
	      CK_BYTE_PTR pSignature,
	      CK_ULONG ulSignatureLen)
{
    st_logf("VerifyFinal\n");
    VERIFY_SESSION_HANDLE(hSession, NULL);
    return CKR_FUNCTION_NOT_SUPPORTED;
}

CK_RV
C_GenerateRandom(CK_SESSION_HANDLE hSession,
		 CK_BYTE_PTR RandomData,
		 CK_ULONG ulRandomLen)
{
    st_logf("GenerateRandom\n");
    VERIFY_SESSION_HANDLE(hSession, NULL);
    return CKR_FUNCTION_NOT_SUPPORTED;
}


CK_FUNCTION_LIST funcs = {
    { 2, 11 },
    C_Initialize,
    C_Finalize,
    C_GetInfo,
    C_GetFunctionList,
    C_GetSlotList,
    C_GetSlotInfo,
    C_GetTokenInfo,
    C_GetMechanismList,
    C_GetMechanismInfo,
    C_InitToken,
    (void *)func_not_supported, /* C_InitPIN */
    (void *)func_not_supported, /* C_SetPIN */
    C_OpenSession,
    C_CloseSession,
    C_CloseAllSessions,
    C_GetSessionInfo,
    (void *)func_not_supported, /* C_GetOperationState */
    (void *)func_not_supported, /* C_SetOperationState */
    C_Login,
    C_Logout,
    (void *)func_not_supported, /* C_CreateObject */
    (void *)func_not_supported, /* C_CopyObject */
    (void *)func_not_supported, /* C_DestroyObject */
    (void *)func_not_supported, /* C_GetObjectSize */
    C_GetAttributeValue,
    (void *)func_not_supported, /* C_SetAttributeValue */
    C_FindObjectsInit,
    C_FindObjects,
    C_FindObjectsFinal,
    C_EncryptInit,
    C_Encrypt,
    C_EncryptUpdate,
    C_EncryptFinal,
    C_DecryptInit,
    C_Decrypt,
    C_DecryptUpdate,
    C_DecryptFinal,
    C_DigestInit,
    (void *)func_not_supported, /* C_Digest */
    (void *)func_not_supported, /* C_DigestUpdate */
    (void *)func_not_supported, /* C_DigestKey */
    (void *)func_not_supported, /* C_DigestFinal */
    C_SignInit,
    C_Sign,
    C_SignUpdate,
    C_SignFinal,
    (void *)func_not_supported, /* C_SignRecoverInit */
    (void *)func_not_supported, /* C_SignRecover */
    C_VerifyInit,
    C_Verify,
    C_VerifyUpdate,
    C_VerifyFinal,
    (void *)func_not_supported, /* C_VerifyRecoverInit */
    (void *)func_not_supported, /* C_VerifyRecover */
    (void *)func_not_supported, /* C_DigestEncryptUpdate */
    (void *)func_not_supported, /* C_DecryptDigestUpdate */
    (void *)func_not_supported, /* C_SignEncryptUpdate */
    (void *)func_not_supported, /* C_DecryptVerifyUpdate */
    (void *)func_not_supported, /* C_GenerateKey */
    (void *)func_not_supported, /* C_GenerateKeyPair */
    (void *)func_not_supported, /* C_WrapKey */
    (void *)func_not_supported, /* C_UnwrapKey */
    (void *)func_not_supported, /* C_DeriveKey */
    (void *)func_not_supported, /* C_SeedRandom */
    C_GenerateRandom,
    (void *)func_not_supported, /* C_GetFunctionStatus */
    (void *)func_not_supported, /* C_CancelFunction */
    (void *)func_not_supported  /* C_WaitForSlotEvent */
};
