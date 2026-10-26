/*
* unicode.c - deterministic Unicode path equivalence keys.
* Stored names are never rewritten: this module only validates UTF-8 and
* computes keys used to reject names that collide under NFC + full casefold.
*/
#include "core.h"
#include <unicode/unorm2.h>
#include <unicode/ustring.h>
#include <unicode/uchar.h>

static int unicode_errno(UErrorCode status) {
if(status==U_MEMORY_ALLOCATION_ERROR) {
errno=ENOMEM;
return -1;
}
if(status==U_INVALID_CHAR_FOUND||status==U_TRUNCATED_CHAR_FOUND||
status==U_ILLEGAL_CHAR_FOUND) {
errno=EILSEQ;
return -1;
}
errno=EINVAL;
return -1;
}

bool mesh_unicode_valid_utf8(const char*text) {
UErrorCode status=U_ZERO_ERROR;
int32_t length=0;
if(!text)
return false;
size_t bytes=strlen(text);
if(bytes>(size_t)INT32_MAX)
return false;
u_strFromUTF8(NULL,0,&length,text,(int32_t)bytes,&status);
return status==U_BUFFER_OVERFLOW_ERROR||U_SUCCESS(status);
}

static UChar*unicode_normalize(const UNormalizer2*normalizer,
const UChar*input,int32_t input_len,int32_t*out_len) {
UErrorCode status=U_ZERO_ERROR;
int32_t needed=unorm2_normalize(normalizer,input,input_len,NULL,0,&status);
if(status!=U_BUFFER_OVERFLOW_ERROR&&U_FAILURE(status)) {
unicode_errno(status);
return NULL;
}
if(needed<0||needed>INT32_MAX-1) {
errno=E2BIG;
return NULL;
}
UChar*out=malloc(((size_t)needed+1)*sizeof(*out));
if(!out)
return NULL;
status=U_ZERO_ERROR;
*out_len=unorm2_normalize(normalizer,input,input_len,out,needed+1,&status);
if(U_FAILURE(status)) {
free(out);
unicode_errno(status);
return NULL;
}
return out;
}

static UChar*unicode_fold(const UChar*input,int32_t input_len,
int32_t*out_len) {
UErrorCode status=U_ZERO_ERROR;
int32_t needed=u_strFoldCase(NULL,0,input,input_len,U_FOLD_CASE_DEFAULT,
&status);
if(status!=U_BUFFER_OVERFLOW_ERROR&&U_FAILURE(status)) {
unicode_errno(status);
return NULL;
}
if(needed<0||needed>INT32_MAX-1) {
errno=E2BIG;
return NULL;
}
UChar*out=malloc(((size_t)needed+1)*sizeof(*out));
if(!out)
return NULL;
status=U_ZERO_ERROR;
*out_len=u_strFoldCase(out,needed+1,input,input_len,U_FOLD_CASE_DEFAULT,
&status);
if(U_FAILURE(status)) {
free(out);
unicode_errno(status);
return NULL;
}
return out;
}

int mesh_unicode_key(const char*text,char**key) {
UErrorCode status=U_ZERO_ERROR;
const UNormalizer2*nfc;
UChar*utf16=NULL,*first=NULL,*folded=NULL,*normalized=NULL;
char*out=NULL;
int32_t utf16_len=0,first_len=0,folded_len=0,normalized_len=0;
int32_t out_len=0;
size_t bytes;
if(!text||!key) {
errno=EINVAL;
return -1;
}
*key=NULL;
bytes=strlen(text);
if(bytes>(size_t)INT32_MAX) {
errno=E2BIG;
return -1;
}
u_strFromUTF8(NULL,0,&utf16_len,text,(int32_t)bytes,&status);
if(status!=U_BUFFER_OVERFLOW_ERROR&&U_FAILURE(status))
return unicode_errno(status);
utf16=malloc(((size_t)utf16_len+1)*sizeof(*utf16));
if(!utf16)
goto done;
status=U_ZERO_ERROR;
u_strFromUTF8(utf16,utf16_len+1,&utf16_len,text,(int32_t)bytes,&status);
if(U_FAILURE(status)) {
unicode_errno(status);
goto done;
}
status=U_ZERO_ERROR;
nfc=unorm2_getNFCInstance(&status);
if(U_FAILURE(status)) {
unicode_errno(status);
goto done;
}
first=unicode_normalize(nfc,utf16,utf16_len,&first_len);
if(!first)
goto done;
folded=unicode_fold(first,first_len,&folded_len);
if(!folded)
goto done;
normalized=unicode_normalize(nfc,folded,folded_len,&normalized_len);
if(!normalized)
goto done;
status=U_ZERO_ERROR;
u_strToUTF8(NULL,0,&out_len,normalized,normalized_len,&status);
if(status!=U_BUFFER_OVERFLOW_ERROR&&U_FAILURE(status)) {
unicode_errno(status);
goto done;
}
out=malloc((size_t)out_len+1);
if(!out)
goto done;
status=U_ZERO_ERROR;
u_strToUTF8(out,out_len+1,&out_len,normalized,normalized_len,&status);
if(U_FAILURE(status)) {
unicode_errno(status);
free(out);
out=NULL;
goto done;
}
out[out_len]='\0';
*key=out;
out=NULL;
done:
free(utf16);
free(first);
free(folded);
free(normalized);
free(out);
return *key?0:-1;
}
