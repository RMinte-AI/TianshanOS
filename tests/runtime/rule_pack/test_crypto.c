#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ts_config_pack.h"
#include "ts_crypto.h"
#include "ts_cert.h"
#include "ts_storage.h"
#include <sys/stat.h>
#include <unistd.h>
#include "ts_api.h"
static char scratch[512];
static const char *mapped(const char *path,char out[1024]) {
 if(!strncmp(path,"/sdcard",7)){assert(scratch[0]);snprintf(out,1024,"%s%s",scratch,path+7);return out;}return path;
}
FILE *fixture_fopen(const char *path,const char *mode){char out[1024];return fopen(mapped(path,out),mode);}
int fixture_stat(const char *path,struct stat *st){char out[1024];return stat(mapped(path,out),st);}
int fixture_mkdir(const char *path,mode_t mode){char out[1024];return mkdir(mapped(path,out),mode);}
esp_err_t ts_storage_write_file(const char *path,const void *data,size_t size) {
 assert(!strncmp(path,"/sdcard/config/",15));FILE *f=fixture_fopen(path,"wb");if(!f)return ESP_FAIL;
 bool ok=fwrite(data,1,size,f)==size;return fclose(f)==0&&ok?ESP_OK:ESP_FAIL;
}
void ts_api_result_error(ts_api_result_t *r,ts_api_result_code_t code,const char *message){r->code=code;r->message=(char*)message;}
void ts_api_result_ok(ts_api_result_t *r,cJSON *data){r->code=TS_API_OK;r->data=data;}
#ifndef PACK_BASELINE
#define PACK_MALLOC malloc
#include "export_api.inc"
#endif
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include <time.h>
static time_t fixture_time;
time_t time(time_t *out){time_t now=fixture_time?fixture_time:TEST_TIME;if(out)*out=now;return now;}
static char *device_key, *device_cert, *ca;
static uint32_t generation=1;
static bool trusted_time=true;
static const char *certificate_after_read;
static esp_err_t certificate_read_error;
static char *read_file(const char *dir,const char *name) {
 char path[512];snprintf(path,sizeof path,"%s/%s",dir,name);
 FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
 char *s=malloc(n+1);assert(fread(s,1,n,f)==(size_t)n);s[n]=0;fclose(f);return s;
}
static esp_err_t copy(const char *s,char *out,size_t *n) {
 if(!s)return ESP_ERR_INVALID_STATE;
 size_t len=strlen(s)+1;if(*n<len)return ESP_ERR_INVALID_SIZE;memcpy(out,s,len);*n=len;return ESP_OK;
}
esp_err_t ts_cert_get_private_key(char *s,size_t *n){return copy(device_key,s,n);}
esp_err_t ts_cert_get_certificate(char *s,size_t *n){if(certificate_read_error)return certificate_read_error;int ret=copy(device_cert,s,n);if(ret==ESP_OK&&certificate_after_read){free(device_cert);device_cert=strdup(certificate_after_read);certificate_after_read=NULL;}return ret;}
esp_err_t ts_cert_get_ca_chain(char *s,size_t *n){return copy(ca,s,n);}
esp_err_t ts_cert_parse_certificate(const char *s,size_t n,ts_cert_info_t *info) {
 mbedtls_x509_crt c;mbedtls_x509_crt_init(&c);memset(info,0,sizeof *info);
 if(mbedtls_x509_crt_parse(&c,(const unsigned char *)s,n)){mbedtls_x509_crt_free(&c);return ESP_FAIL;}
 for(mbedtls_x509_name *p=&c.subject;p;p=p->next){
  char *out=NULL;size_t cap=0;
  if(!MBEDTLS_OID_CMP(MBEDTLS_OID_AT_ORG_UNIT,&p->oid)){out=info->subject_ou;cap=sizeof info->subject_ou;}
  if(!MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN,&p->oid)){out=info->subject_cn;cap=sizeof info->subject_cn;}
  if(out){size_t len=p->val.len<cap-1?p->val.len:cap-1;memcpy(out,p->val.p,len);out[len]=0;}
 }
 mbedtls_x509_crt_free(&c);return ESP_OK;
}
esp_err_t ts_cert_get_info(ts_cert_info_t *i){return device_cert?ts_cert_parse_certificate(device_cert,strlen(device_cert)+1,i):ESP_ERR_INVALID_STATE;}
esp_err_t ts_cert_get_status(ts_cert_pki_status_t *s){memset(s,0,sizeof *s);s->time_ready=trusted_time;s->generation=generation;return ESP_OK;}
esp_err_t ts_cert_get_pack_snapshot(ts_cert_snapshot_t *s){
 memset(s,0,sizeof *s);s->key=strdup(device_key);s->certificate=strdup(device_cert);s->ca=strdup(ca);s->generation=generation;return ESP_OK;
}
void ts_cert_free_snapshot(ts_cert_snapshot_t *s){if(s->key)memset(s->key,0,strlen(s->key));free(s->key);free(s->certificate);free(s->ca);memset(s,0,sizeof *s);}

#ifndef PACK_BASELINE
static void expected_fingerprint(const char *pem,char fp[65]) {
 size_t len=strlen(pem);while(len&&strchr(" \t\r\n",pem[len-1]))--len;
 unsigned char hash[32];assert(ts_crypto_hash(TS_HASH_SHA256,pem,len,hash,32)==ESP_OK);
 assert(ts_crypto_hex_encode(hash,32,fp,65)==ESP_OK);
}
static void assert_export(const char *pem) {
 ts_api_result_t response={0};assert(api_config_pack_export_cert(NULL,&response)==ESP_OK&&response.code==TS_API_OK);
 assert(!strcmp(cJSON_GetObjectItem(response.data,"certificate")->valuestring,pem));
 char expected[65];expected_fingerprint(pem,expected);
 assert(!strcmp(cJSON_GetObjectItem(response.data,"fingerprint")->valuestring,expected));cJSON_Delete(response.data);
}
static void ordinary_consumers(const char *bytes,size_t size,ts_config_pack_result_t expected,const char *content) {
 ts_config_pack_t *pack=NULL;assert(ts_config_pack_load_mem(bytes,size,&pack)==expected);
 if(pack){assert(expected==TS_CONFIG_PACK_OK&&pack->sig_info.valid&&!strcmp(pack->content,content));ts_config_pack_free(pack);}
 char input[1024];snprintf(input,sizeof(input),"%s/input.tscfg",scratch);FILE *f=fopen(input,"wb");assert(f&&fwrite(bytes,1,size,f)==size);assert(!fclose(f));
 ts_config_pack_metadata_t metadata={0};assert(ts_config_pack_validate_file(input,&metadata)==expected);
 char saved[128]={0};assert(ts_config_pack_import(bytes,size,&metadata,saved,sizeof(saved))==expected);
 if(expected==TS_CONFIG_PACK_OK){assert(metadata.sig_info.valid);ts_config_pack_t *loaded=NULL;assert(ts_config_pack_load(saved,&loaded)==TS_CONFIG_PACK_OK);assert(!strcmp(loaded->content,content));ts_config_pack_free(loaded);}
 unlink(input);
}
static void ordinary_identity_cases(const char *dir,const char *certificate_name,const char *key_name) {
 char template[]="/tmp/tianshan-ordinary-pack-XXXXXX";assert(mkdtemp(template));strcpy(scratch,template);
 char *a_cert=strdup(device_cert),*a_key=strdup(device_key),*b_cert=read_file(dir,certificate_name),*b_key=read_file(dir,key_name);
 const char *content="{\"ordinary\":true,\"value\":23}";
 char *a_bytes=NULL,*b_bytes=NULL;size_t a_len=0,b_len=0;
 ts_config_pack_export_opts_t options={.recipient_cert_pem=a_cert,.recipient_cert_len=strlen(a_cert)+1};
 assert(ts_config_pack_create("ordinary",content,strlen(content),&options,&a_bytes,&a_len)==TS_CONFIG_PACK_OK);
 ordinary_consumers(a_bytes,a_len,TS_CONFIG_PACK_OK,content);assert_export(a_cert);
 options.recipient_cert_pem=b_cert;options.recipient_cert_len=strlen(b_cert)+1;
 assert(ts_config_pack_create("ordinary",content,strlen(content),&options,&b_bytes,&b_len)==TS_CONFIG_PACK_OK);
 free(device_cert);free(device_key);device_cert=strdup(b_cert);device_key=strdup(b_key);
 char fp[65],expected[65];expected_fingerprint(b_cert,expected);
 assert(ts_config_pack_get_cert_fingerprint(fp,sizeof(fp))==ESP_OK&&!strcmp(fp,expected));assert_export(b_cert);
 ordinary_consumers(b_bytes,b_len,TS_CONFIG_PACK_OK,content);
 ordinary_consumers(a_bytes,a_len,TS_CONFIG_PACK_ERR_RECIPIENT,content);
 cJSON *bad=cJSON_Parse(b_bytes);cJSON *signature=cJSON_GetObjectItem(cJSON_GetObjectItem(bad,"signature"),"signature");signature->valuestring[0]=signature->valuestring[0]=='A'?'B':'A';char *invalid=cJSON_PrintUnformatted(bad);
 ordinary_consumers(invalid,strlen(invalid),TS_CONFIG_PACK_ERR_SIGNATURE,content);free(invalid);cJSON_Delete(bad);
 /* Import/validate remain signature-only; memory load still performs real decryption. */
 free(device_key);device_key=read_file(dir,"other.key");ts_config_pack_t *pack=NULL;
 assert(ts_config_pack_load_mem(b_bytes,b_len,&pack)==TS_CONFIG_PACK_ERR_DECRYPT&&!pack);
 free(device_key);device_key=strdup(b_key);
 free(device_cert);device_cert=NULL;assert(ts_config_pack_get_cert_fingerprint(fp,sizeof(fp))!=ESP_OK);
 ts_api_result_t response={0};assert(api_config_pack_export_cert(NULL,&response)!=ESP_OK&&!response.data);
 ordinary_consumers(b_bytes,b_len,TS_CONFIG_PACK_ERR_RECIPIENT,content);
 device_cert=strdup(b_cert);
 certificate_read_error=ESP_ERR_NO_MEM;assert(ts_config_pack_get_cert_fingerprint(fp,sizeof(fp))==ESP_ERR_NO_MEM);
 ordinary_consumers(b_bytes,b_len,TS_CONFIG_PACK_ERR_NO_MEM,content);certificate_read_error=ESP_OK;
 bool old_time=trusted_time;char *old_ca=ca;trusted_time=false;ca=NULL;
 assert(ts_config_pack_get_cert_fingerprint(fp,sizeof(fp))==ESP_OK);assert_export(b_cert);
 /* Simulate replacement immediately after the export read: response stays on A. */
 free(device_cert);device_cert=strdup(a_cert);certificate_after_read=b_cert;assert_export(a_cert);assert_export(b_cert);
 trusted_time=old_time;ca=old_ca;
 free(device_key);free(device_cert);device_key=strdup(a_key);device_cert=strdup(a_cert);
 free(a_cert);free(a_key);free(b_cert);free(b_key);free(a_bytes);free(b_bytes);
 char path[1024];snprintf(path,sizeof(path),"%s/config/ordinary.tscfg",scratch);assert(!unlink(path));snprintf(path,sizeof(path),"%s/config",scratch);assert(!rmdir(path));assert(!rmdir(scratch));scratch[0]=0;
 puts("PASS F73 ordinary production load/import/file validation with real signatures/ECDH/AES: A, replacement B, reject old/other identity, bad signature/decryption, delete/reinstall; actual export API same-PEM response, no CA/time gate");
}
#endif
#ifdef PACK_EXPORT_PAYLOADS
static void export_payload_cases(const char *dir,const char *path)
{
 FILE *file=fopen(path,"rb");assert(file);fseek(file,0,SEEK_END);long size=ftell(file);assert(size>0);rewind(file);
 char *json=malloc(size+1);assert(json&&fread(json,1,size,file)==(size_t)size);json[size]=0;assert(!fclose(file));
 cJSON *records=cJSON_Parse(json);free(json);assert(cJSON_IsArray(records)&&cJSON_GetArraySize(records)>0);
 cJSON *record;unsigned count=0;
 cJSON_ArrayForEach(record,records){
  const char *name=cJSON_GetStringValue(cJSON_GetObjectItem(record,"name")),*description=cJSON_GetStringValue(cJSON_GetObjectItem(record,"description"));
  const char *content=cJSON_GetStringValue(cJSON_GetObjectItem(record,"content")),*hash=cJSON_GetStringValue(cJSON_GetObjectItem(record,"sha256"));
  const char *target=cJSON_GetStringValue(cJSON_GetObjectItem(record,"recipient"));assert(name&&description&&content&&hash&&target);
  size_t content_len=strlen(content);assert(content_len==(size_t)cJSON_GetObjectItem(record,"length")->valuedouble);
  unsigned char digest[32];char hex[65];assert(ts_crypto_hash(TS_HASH_SHA256,content,content_len,digest,32)==ESP_OK&&ts_crypto_hex_encode(digest,32,hex,65)==ESP_OK&&!strcmp(hex,hash));
  free(device_key);free(device_cert);device_key=read_file(dir,"developer.key");device_cert=read_file(dir,"developer.pem");
  char *recipient=!strcmp(target,"self")?strdup(device_cert):read_file(dir,"recipient.pem");
  ts_config_pack_export_opts_t opts={.recipient_cert_pem=recipient,.recipient_cert_len=strlen(recipient),.description=description};
  char *bytes=NULL;size_t length=0;assert(ts_config_pack_create(name,content,content_len,&opts,&bytes,&length)==TS_CONFIG_PACK_OK&&length==strlen(bytes));
  cJSON *wire=ts_config_pack_parse_json(bytes,length);assert(wire);const cJSON *metadata=cJSON_GetObjectItem(wire,"metadata");
  assert(cJSON_IsString(cJSON_GetObjectItem(metadata,"name"))&&!strcmp(cJSON_GetObjectItem(metadata,"name")->valuestring,name));
  assert(cJSON_IsString(cJSON_GetObjectItem(metadata,"description"))&&!strcmp(cJSON_GetObjectItem(metadata,"description")->valuestring,description));cJSON_Delete(wire);
  if(strcmp(target,"self")){free(device_key);free(device_cert);device_key=read_file(dir,"recipient.key");device_cert=strdup(recipient);}
  ts_config_pack_t *pack=NULL;assert(ts_config_pack_load_verified_mem(bytes,length,NULL,&pack,NULL,NULL)==TS_CONFIG_PACK_OK&&pack);
  assert(pack->content_len==content_len&&!memcmp(pack->content,content,content_len));
  assert(ts_crypto_hash(TS_HASH_SHA256,pack->content,pack->content_len,digest,32)==ESP_OK&&ts_crypto_hex_encode(digest,32,hex,65)==ESP_OK&&!strcmp(hex,hash));
  printf("PASS export payload %s %s %zu %s: actual signed/encrypted create + verified decrypt, exact content/hash; original outbound metadata\n",name,target,content_len,hash);
  ts_config_pack_free(pack);free(bytes);free(recipient);++count;
 }
 cJSON_Delete(records);printf("PASS %u exact configuration-owner export payload records through production PKI crypto\n",count);
}
#endif
int main(int argc,char **argv) {
#ifdef PACK_EXPORT_PAYLOADS
 assert(argc==3);
#else
 assert(argc==2);
#endif
 const char *dir=argv[1];
 device_key=read_file(dir,"developer.key");device_cert=read_file(dir,"developer.pem");ca=read_file(dir,"root.pem");
#ifdef PACK_BASELINE
 assert(ts_config_pack_init()==ESP_OK);
#else
 char *initial=device_cert;device_cert=NULL;assert(ts_config_pack_init()==ESP_OK);
 char initial_fp[65];assert(ts_config_pack_get_cert_fingerprint(initial_fp,sizeof(initial_fp))!=ESP_OK);
 device_cert=initial;char expected[65];expected_fingerprint(device_cert,expected);
 assert(ts_config_pack_get_cert_fingerprint(initial_fp,sizeof(initial_fp))==ESP_OK&&!strcmp(initial_fp,expected));
 mbedtls_x509_crt identity;mbedtls_x509_crt_init(&identity);assert(!mbedtls_x509_crt_parse(&identity,(unsigned char*)device_cert,strlen(device_cert)+1));
 unsigned char der_hash[32];char der_fp[65];assert(ts_crypto_hash(TS_HASH_SHA256,identity.raw.p,identity.raw.len,der_hash,32)==ESP_OK);assert(ts_crypto_hex_encode(der_hash,32,der_fp,65)==ESP_OK&&strcmp(initial_fp,der_fp));mbedtls_x509_crt_free(&identity);
 puts("PASS F73 first certificate after init is current; historical trimmed-PEM identity differs from DER");
 ordinary_identity_cases(dir,"developer-renewed.pem","developer.key");
 ordinary_identity_cases(dir,"recipient.pem","recipient.key");
#endif
#ifdef PACK_EXPORT_PAYLOADS
 export_payload_cases(dir,argv[2]);free(device_key);free(device_cert);free(ca);return 0;
#endif
 char *recipient=read_file(dir,"recipient.pem");
 const char *content="{\"type\":\"automation_rule\",\"rule\":{\"id\":\"night\",\"name\":\"Night\",\"enabled\":true,\"actions\":[{\"type\":\"log\",\"message\":\"test\"}]}}";
 ts_config_pack_export_opts_t opts={.recipient_cert_pem=recipient,.recipient_cert_len=strlen(recipient)};
#ifndef PACK_BASELINE
 char *exact=malloc(opts.recipient_cert_len);memcpy(exact,recipient,opts.recipient_cert_len);opts.recipient_cert_pem=exact;
#endif
 char *bytes=NULL;size_t length=0;
 ts_config_pack_result_t made=ts_config_pack_create("ignored-name",content,strlen(content),&opts,&bytes,&length);
#ifndef PACK_BASELINE
 free(exact);
#endif
#ifdef PACK_BASELINE
 printf("baseline explicit recipient PEM length: create=%d (expected OK)\n",made);
 fflush(stdout);
 free(bytes);free(recipient);free(device_key);free(device_cert);free(ca);
 assert(made==TS_CONFIG_PACK_OK);
#elif defined(PACK_UNCONFIGURED)
 assert(made==TS_CONFIG_PACK_OK);
 free(device_key);free(device_cert);device_key=read_file(dir,"recipient.key");device_cert=strdup(recipient);
 ts_config_pack_t *pack=NULL;
 assert(ts_config_pack_load_verified_mem(bytes,length,NULL,&pack,NULL,NULL)==TS_CONFIG_PACK_ERR_TRUST_NOT_CONFIGURED&&!pack);
 free(bytes);free(recipient);free(device_key);free(device_cert);free(ca);
 puts("PASS unconfigured signing root refuses a cryptographically valid package");
#else
 assert(made==TS_CONFIG_PACK_OK&&bytes&&length==strlen(bytes));
 ts_config_pack_export_opts_t self_opts={.recipient_cert_pem=device_cert,.recipient_cert_len=strlen(device_cert)+1};
 char *self=NULL;size_t self_len=0;ts_config_pack_t *self_pack=NULL;
 assert(ts_config_pack_create("self",content,strlen(content),&self_opts,&self,&self_len)==TS_CONFIG_PACK_OK);
 assert(ts_config_pack_load_verified_mem(self,self_len,NULL,&self_pack,NULL,NULL)==TS_CONFIG_PACK_OK&&!strcmp(self_pack->content,content));
 ts_config_pack_free(self_pack);free(self);
 free(device_key);free(device_cert);device_key=read_file(dir,"recipient.key");device_cert=strdup(recipient);
 ts_config_pack_t *pack=NULL;ts_config_pack_acceptance_t accepted={0};uint32_t gen=0;
 assert(ts_config_pack_load_verified_mem(bytes,length,NULL,&pack,&accepted,&gen)==TS_CONFIG_PACK_OK);
 assert(gen==generation&&!strcmp(pack->content,content));ts_config_pack_free(pack);
 /* Expiry is checked when first accepting; accepted exact bytes may reload. */
 fixture_time=TEST_TIME+4000LL*86400;
 assert(ts_config_pack_load_verified_mem(bytes,length,NULL,&pack,NULL,NULL)==TS_CONFIG_PACK_ERR_CERT_CHAIN);
 assert(ts_config_pack_load_verified_mem(bytes,length,&accepted,&pack,NULL,NULL)==TS_CONFIG_PACK_OK);ts_config_pack_free(pack);
 fixture_time=0;
 ts_config_pack_acceptance_t changed=accepted;changed.policy++;
 assert(ts_config_pack_load_verified_mem(bytes,length,&changed,&pack,NULL,NULL)==TS_CONFIG_PACK_ERR_CERT_CHAIN);
 changed=accepted;changed.root_sha256[0]^=1;
 assert(ts_config_pack_load_verified_mem(bytes,length,&changed,&pack,NULL,NULL)==TS_CONFIG_PACK_ERR_CERT_CHAIN);
 trusted_time=false;
 assert(ts_config_pack_load_verified_mem(bytes,length,NULL,&pack,NULL,NULL)==TS_CONFIG_PACK_ERR_TIME_UNVERIFIED);
 assert(ts_config_pack_load_verified_mem(bytes,length,&accepted,&pack,NULL,NULL)==TS_CONFIG_PACK_OK);ts_config_pack_free(pack);
 trusted_time=true;
 char *save=device_cert;device_cert=read_file(dir,"other.pem");
 assert(ts_config_pack_load_verified_mem(bytes,length,NULL,&pack,NULL,NULL)==TS_CONFIG_PACK_ERR_RECIPIENT);
 free(device_cert);device_cert=save;
 const char *unauthorized[]={"not-developer.pem","tls-developer.pem","no-sign.pem","p384.pem","duplicate-role.pem"};
 for(unsigned i=0;i<sizeof unauthorized/sizeof *unauthorized;++i){
  cJSON *j=cJSON_Parse(bytes);char *certificate=read_file(dir,unauthorized[i]);
  assert(cJSON_ReplaceItemInObject(cJSON_GetObjectItem(j,"signature"),"signer_certificate",cJSON_CreateString(certificate)));
  char *bad=cJSON_PrintUnformatted(j);
  assert(ts_config_pack_load_verified_mem(bad,strlen(bad),NULL,&pack,NULL,NULL)==TS_CONFIG_PACK_ERR_SIGNER_ROLE);
  free(certificate);free(bad);cJSON_Delete(j);
 }
 char *old_ca=ca,*alternate=read_file(dir,"alternate-root.pem");
 ca=malloc(strlen(old_ca)+strlen(alternate)+1);strcpy(ca,old_ca);strcat(ca,alternate);
 cJSON *foreign=cJSON_Parse(bytes);char *foreign_cert=read_file(dir,"alternate-developer.pem");
 assert(cJSON_ReplaceItemInObject(cJSON_GetObjectItem(foreign,"signature"),"signer_certificate",cJSON_CreateString(foreign_cert)));
 char *foreign_bytes=cJSON_PrintUnformatted(foreign);
 assert(ts_config_pack_load_verified_mem(foreign_bytes,strlen(foreign_bytes),NULL,&pack,NULL,NULL)==TS_CONFIG_PACK_ERR_CERT_CHAIN);
 free(foreign_bytes);free(foreign_cert);cJSON_Delete(foreign);free(ca);free(alternate);ca=old_ca;
 cJSON *j=cJSON_Parse(bytes);cJSON *enc=cJSON_GetObjectItem(j,"encryption");
 cJSON_ReplaceItemInObject(enc,"salt",cJSON_CreateNumber(7));char *bad=cJSON_PrintUnformatted(j);
 assert(ts_config_pack_load_verified_mem(bad,strlen(bad),NULL,&pack,NULL,NULL)==TS_CONFIG_PACK_ERR_PARSE);
 free(bad);cJSON_Delete(j);
 j=cJSON_Parse(bytes);enc=cJSON_GetObjectItem(j,"encryption");
 char *tag=cJSON_GetObjectItem(enc,"tag")->valuestring;tag[0]=tag[0]=='A'?'B':'A';bad=cJSON_PrintUnformatted(j);
 assert(ts_config_pack_load_verified_mem(bad,strlen(bad),NULL,&pack,NULL,NULL)==TS_CONFIG_PACK_ERR_DECRYPT);
 free(bad);cJSON_Delete(j);
 j=cJSON_Parse(bytes);enc=cJSON_GetObjectItem(j,"encryption");
 cJSON_ReplaceItemInObject(enc,"iv",cJSON_CreateString("AA=="));bad=cJSON_PrintUnformatted(j);
 assert(ts_config_pack_load_verified_mem(bad,strlen(bad),NULL,&pack,NULL,NULL)==TS_CONFIG_PACK_ERR_PARSE);
 free(bad);cJSON_Delete(j);
 assert(!ts_config_pack_parse_json("{\"x\":1,\"x\":2}",13));
 assert(!ts_config_pack_parse_json("{} {}",5));
 assert(!ts_config_pack_parse_json("{\"x\":\"\\u0000\"}",14));
 assert(!ts_config_pack_parse_import_request("{\"tscfg\":\"ok\\u0000tail\"}",24));
 assert(!ts_config_pack_parse_import_request("{\"preview\":true,\"preview\":false}",32));
 cJSON *empty=ts_config_pack_parse_json("{}",2);assert(empty);cJSON_Delete(empty);
 j=cJSON_Parse(bytes);char *cipher=cJSON_GetObjectItem(j,"payload")->valuestring;
 cipher[0]=cipher[0]=='A'?'B':'A';bad=cJSON_PrintUnformatted(j);
 assert(ts_config_pack_load_verified_mem(bad,strlen(bad),NULL,&pack,NULL,NULL)==TS_CONFIG_PACK_ERR_SIGNATURE);
 free(bad);cJSON_Delete(j);
 free(bytes);free(recipient);free(device_key);free(device_cert);free(ca);
 puts("PASS real production create/trust/signature/ECDH/HKDF/GCM, cross-device PEM, offline accepted reload, malformed inputs");
#endif
 return 0;
}
