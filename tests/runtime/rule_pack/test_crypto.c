#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ts_config_pack.h"
#include "ts_crypto.h"
#include "ts_cert.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include <time.h>
static time_t fixture_time;
time_t time(time_t *out){time_t now=fixture_time?fixture_time:TEST_TIME;if(out)*out=now;return now;}
static char *device_key, *device_cert, *ca;
static uint32_t generation=1;
static bool trusted_time=true;
static char *read_file(const char *dir,const char *name) {
 char path[512];snprintf(path,sizeof path,"%s/%s",dir,name);
 FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
 char *s=malloc(n+1);assert(fread(s,1,n,f)==(size_t)n);s[n]=0;fclose(f);return s;
}
static esp_err_t copy(const char *s,char *out,size_t *n) {
 size_t len=strlen(s)+1;if(*n<len)return ESP_ERR_INVALID_SIZE;memcpy(out,s,len);*n=len;return ESP_OK;
}
esp_err_t ts_cert_get_private_key(char *s,size_t *n){return copy(device_key,s,n);}
esp_err_t ts_cert_get_certificate(char *s,size_t *n){return copy(device_cert,s,n);}
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
esp_err_t ts_cert_get_info(ts_cert_info_t *i){return ts_cert_parse_certificate(device_cert,strlen(device_cert)+1,i);}
esp_err_t ts_cert_get_status(ts_cert_pki_status_t *s){memset(s,0,sizeof *s);s->time_ready=trusted_time;s->generation=generation;return ESP_OK;}
esp_err_t ts_cert_get_pack_snapshot(ts_cert_snapshot_t *s){
 memset(s,0,sizeof *s);s->key=strdup(device_key);s->certificate=strdup(device_cert);s->ca=strdup(ca);s->generation=generation;return ESP_OK;
}
void ts_cert_free_snapshot(ts_cert_snapshot_t *s){if(s->key)memset(s->key,0,strlen(s->key));free(s->key);free(s->certificate);free(s->ca);memset(s,0,sizeof *s);}
int main(int argc,char **argv) {
 assert(argc==2);const char *dir=argv[1];
 device_key=read_file(dir,"developer.key");device_cert=read_file(dir,"developer.pem");ca=read_file(dir,"root.pem");
 assert(ts_config_pack_init()==ESP_OK);
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
