/**
 * @brief		
 * @details		
 * @date		azenk@2019-01-10
 **/
/* Includes ------------------------------------------------------------------*/
#include <stdlib.h>
#include <string.h>
#include "system.h"
#include "config_protocol.h"
#include "jiffy.h"
#include "dlms_association.h"
#include "dlms_application.h"
#include "dlms_utilities.h"
#include "axdr.h"
#include "mbedtls/gcm.h"

/* Private typedef -----------------------------------------------------------*/
/**	
  * @brief AP
  */
struct __ap
{
    uint16_t ld;//logic device
    uint8_t conformance[3];//支持的 conformance block
    uint8_t suit;//支持的 object list
};

/**	
  * @brief Association Source Diagnostics
  */
enum __asso_diagnose
{
    SUCCESS_NOSEC_LLS = 0,
    FAILURE_CONTEXT_NAME,
    FAILURE_NO_REASION,
	FAILURE_CALLING_TITLE,
	FAILURE_AUTH = 0x0D,
    SUCCESS_HLS = 0x0E,
};

/**	
  * @brief User information
  */
struct __user_info
{
    uint8_t sc;
    uint8_t dedkey[32+2];
    uint8_t response;
    uint8_t quality;
    uint8_t version;
    uint16_t max_pdu;
};

/**	
  * @brief Association object
  */
struct __dlms_association
{
    enum __asso_status status;
    enum __dlms_access_level level;
    enum __asso_diagnose diagnose;
    struct __ap ap;
    uint16_t session;
    struct __object_identifier appl_name;
    struct __object_identifier mech_name;
    uint8_t callingtitle[8+2];
    uint8_t localtitle[8+2];
    uint8_t akey[32+2];//报文认证密钥
    uint8_t ekey[32+2];//报文加密密钥
    uint8_t ssprikey[48+2];//服务器签名私钥
    uint8_t cspubkey[96+2];//客户端验签公钥
    uint8_t ctos[64+2];
    uint8_t stoc[64+2];
    uint8_t sc;
    uint32_t fc;
    struct __user_info info;
    void *appl;
    uint16_t sz_appl;
};

/**	
  * @brief Association request type
  */
enum __asso_reqs
{
    AARQ = 0x60,
    AARE,
    RLRQ,
    RLRE,
};

/**	
  * @brief Decode AARQ request frame
  */
struct __aarq_request
{
    const uint8_t *protocol_version;
    const uint8_t *application_context_name;
    const uint8_t *calling_AP_title;
    const uint8_t *calling_AE_qualifier;
    const uint8_t *calling_AP_invocation_id;
    const uint8_t *calling_AE_invocation_id;
    const uint8_t *sender_acse_requirements;
    const uint8_t *mechanism_name;
    const uint8_t *calling_authentication_value;
    const uint8_t *implementation_information;
    const uint8_t *user_information;
};

/* Private define ------------------------------------------------------------*/
//DLMS 配置参数

//DLMS 最大APDU长度
#define DLMS_CONFIG_MAX_APDU                    ((uint16_t)(512)) //>=32

//DLMS 同时最多支持的ASSOCIATION数量
#define DLMS_CONFIG_MAX_ASSO                    ((uint8_t)(6))

//定义外部接口
//COSEM层请求（info, length, buffer, max buffer length, filled buffer length）
#define DLMS_CONFIG_COSEM_REQUEST(i,l,b,m,f)    dlms_appl_entrance(i,l,b,m,f)
//加载密码（info）
#define DLMS_CONFIG_LOAD_PASSWD(i)              dlms_util_load_passwd(i)
//加载认证密钥（info）
#define DLMS_CONFIG_LOAD_AKEY(i)                dlms_util_load_akey(i)
//加载加密密钥（info）
#define DLMS_CONFIG_LOAD_EKEY(i)                dlms_util_load_uekey(i)
//加载服务器签名私钥（info）
#define DLMS_CONFIG_LOAD_SSKEY(i)				dlms_util_load_ssprikey(i)
//加载客户端验签公钥（info）
#define DLMS_CONFIG_LOAD_CSKEY(i)				dlms_util_load_cspubkey(i)
//加载本机system title（info）
#define DLMS_CONFIG_LOAD_TITLE(i)               dlms_util_load_title(i)
//加载管理维护密码（info）
#define DLMS_CONFIG_LOAD_PASSWD_MANAGE(i)       dlms_util_load_management_passwd(i)

/* Private macro -------------------------------------------------------------*/
#define DLMS_AP_AMOUNT                          ((uint8_t)(sizeof(ap_support_list) / sizeof(struct __ap)))

/* Private variables ---------------------------------------------------------*/
/**	
  * @brief 已注册支持的AP
  */
static const struct __ap ap_support_list[] = 
{
    //内容依次为：
    //SAP
    //支持的 conformance
    //使用的 suit
    {0x3ffc,{0x00, 0x30, 0x1D},(1<<7)},//suit 8 管理维护专用，不可屏蔽
    {0x0001,{0x00, 0x10, 0x11},(1<<0)},
    {0x0002,{0x00, 0x10, 0x11},(1<<1)},
    {0x0003,{0x00, 0x30, 0x1D},(1<<2)},
};

/**	
  * @brief 已经建立的连接对象
  */
static struct __dlms_association *asso_list[DLMS_CONFIG_MAX_ASSO] = {0};

/**	
  * @brief 当前正在处理的连接对象
  */
static struct __dlms_association *asso_current = (void *)0;

/**	
  * @brief 标记密钥需要在完成本次通信之后需要刷新
  */
static uint8_t key_is_eliminate = 0;

/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/
/**
  * @brief 索引 AARQ 报文
  */
static void parse_aarq_frame(const uint8_t *info, uint16_t length, struct __aarq_request *request)
{
    uint16_t frame_length = 0;
    uint16_t frame_decoded = 0;
    
    if(!request)
    {
        return;
    }
    
    frame_decoded = 1 + axdr.length.decode((info + 1), &frame_length);
    
    if(frame_decoded < 2)
    {
        return;
    }
    
    heap.set(request, 0, sizeof(struct __aarq_request));
    
    if(length != (frame_length + frame_decoded))
    {
        return;
    }
    
    while(1)
    {
        if((frame_decoded >= length) || (frame_decoded >= frame_length))
        {
            break;
        }
        
        switch(*(info + frame_decoded))
        {
            case 0x80: //protocol-version,implicit bit-string
			{
                request->protocol_version = (info + frame_decoded);
                frame_decoded += *(info + frame_decoded + 1) + 2;
				break;
			}
            case 0xA1: //APPLICATION-CONTEXT-NAME
			{
                request->application_context_name = (info + frame_decoded);
                frame_decoded += *(info + frame_decoded + 1) + 2;
				break;
			}
            case 0xA6: //AP-title(calling)
			{
                request->calling_AP_title = (info + frame_decoded);
                frame_decoded += *(info + frame_decoded + 1) + 2;
				break;
			}
            case 0xA7: //AE-qualifier-identifier(calling)
			{
                request->calling_AE_qualifier = (info + frame_decoded);
                frame_decoded += *(info + frame_decoded + 1) + 2;
				break;
			}
            case 0xA8: //AP-invocation-identifier(calling)
			{
                request->calling_AP_invocation_id = (info + frame_decoded);
                frame_decoded += *(info + frame_decoded + 1) + 2;
				break;
			}
            case 0xA9: //AE-invocation-identifier(calling)
			{
                request->calling_AE_invocation_id = (info + frame_decoded);
                frame_decoded += *(info + frame_decoded + 1) + 2;
				break;
			}
            case 0x8A: //ACSE-requirement
			{
                request->sender_acse_requirements = (info + frame_decoded);
                frame_decoded += *(info + frame_decoded + 1) + 2;
				break;
			}
            case 0x8B: //Mechanism-name
			{
                request->mechanism_name = (info + frame_decoded);
                frame_decoded += *(info + frame_decoded + 1) + 2;
				break;
			}
            case 0x9D: //implementation-information
            case 0xBD: //按绿皮书应是0x9D,而CTI软件里是0xBD
			{
                request->implementation_information = (info + frame_decoded);
                frame_decoded += *(info + frame_decoded + 1) + 2;
				break;
			}
            case 0xAC: //Authentication-value
			{
                request->calling_authentication_value = (info + frame_decoded);
                frame_decoded += *(info + frame_decoded + 1) + 2;
				break;
            }
            case 0xBE: //Association-information
			{
                request->user_information = (info + frame_decoded);
                frame_decoded += *(info + frame_decoded + 1) + 2;
				break;
			}
            default: //error
			{
                frame_decoded += *(info + frame_decoded + 1) + 2;
				break;
			}
        }
    }
}

/**
  * @brief 组包 ObjectIdentifier
  */
static void encode_object_identifier(const uint8_t *info, struct __object_identifier *object_identifier)
{
    uint16_t temp;
    
    if(!info)
    {
        return;
    }
    
    if(!object_identifier)
    {
        return;
    }
    
    heap.set(object_identifier, 0, sizeof(struct __object_identifier));
    
    object_identifier->joint_iso_ctt = *(info + 0) / 40;
    object_identifier->country = *(info + 0) % 40;
    temp = *(info + 1);
    temp <<= 8;
    temp += *(info + 2);
    
    if(temp & 0x0100)
    {
        object_identifier->name = (*(info + 2) | 0x80);
        temp &= 0x7fff;
        temp >>= 1;
        object_identifier->name += (temp & 0xff00);
    }
    else
    {
        object_identifier->name = (temp & 0x7fff);
    }
    
    object_identifier->organization = *(info + 3);
    object_identifier->ua = *(info + 4);
    object_identifier->context = *(info + 5);
    object_identifier->id = *(info + 6);
}

/**
  * @brief 解析 ObjectIdentifier, 输出 7字节
  */
static void decode_object_identifier(const struct __object_identifier *object_identifier, uint8_t *info)
{
    if(!info)
    {
        return;
    }
    
    if(!object_identifier)
    {
        return;
    }
    
    heap.set(info, 0, 7);
    
    *(info + 0) = object_identifier->joint_iso_ctt * 40 + object_identifier->country;

    if(object_identifier->name & 0x0080)
    {
        *(info + 2) = (uint8_t)(object_identifier->name & 0x007f);
        *(info + 1) = (uint8_t)(object_identifier->name >> 8);
        *(info + 1) <<= 1;
        *(info + 1) |= 0x81;
    }
    else
    {
        *(info + 2) = (uint8_t)(object_identifier->name & 0x007f);
        *(info + 1) = (uint8_t)(object_identifier->name >> 8);
        *(info + 1) |= 0x80;
    }

    *(info + 3) = object_identifier->organization;
    *(info + 4) = object_identifier->ua;
    *(info + 5) = object_identifier->context;
    *(info + 6) = object_identifier->id;
}

/**
  * @brief 组包 UserInformation
  */
static void parse_user_information(const uint8_t *info, struct __dlms_association *asso)
{
    uint8_t offset;
    uint8_t cnt;
    const uint8_t *msg = info;
    uint8_t *output = (void *)0;
    uint8_t *add = (void *)0;
    uint8_t *iv = (void *)0;
    mbedtls_gcm_context ctx;
    int ret = 0;
    
    if(!asso)
    {
        return;
    }
    
    if(!info)
    {
        heap.set(asso->ap.conformance, 0, sizeof(asso->ap.conformance));
        return;
    }
    
    if(((info[4] == 0x21) && ((info[5] + 4) == info[1])) || \
		((info[4] == 0xDB) && (info[5] == 0x08) && ((info[3] + 2) == info[1])))
    {
		//加载 local_AP_title
		DLMS_CONFIG_LOAD_TITLE(asso->localtitle);
		//加载 ekay
		DLMS_CONFIG_LOAD_EKEY(asso->ekey);
		//加载 akey
		DLMS_CONFIG_LOAD_AKEY(asso->akey);
    	
		if(info[4] == 0x21)
		{
			asso->info.sc = info[6];
		}
		else
		{
			asso->info.sc = info[15];
		}
        
        mbedtls_gcm_init(&ctx);
        ret = mbedtls_gcm_setkey(&ctx,
                                 MBEDTLS_CIPHER_ID_AES,
                                 &asso->ekey[2],
                                 asso->ekey[1]*8);
        
        switch(asso->info.sc)
        {
            case 0x10:
            {
                //仅认证
                output = heap.dalloc(4);
                if(!output)
                {
                    ret = -1;
                    break;
                }
                
                add = heap.dalloc(info[5] + asso->akey[1]);
                if(!add)
                {
                    ret = -1;
                    break;
                }
                add[0] = asso->info.sc;
                heap.copy(&add[1], &asso->akey[2], asso->akey[1]);
                heap.copy(&add[1+asso->akey[1]], &info[11], (info[5] - 5 - 12));
                
                iv = heap.dalloc(12);
                if(!iv)
                {
                    ret = -1;
                    break;
                }
                heap.copy(iv, &asso->callingtitle[2], 8);
                heap.copy(&iv[8], &info[7], 4);
                
                if(ret)
                {
                    break;
                }
                
                ret = mbedtls_gcm_auth_decrypt(&ctx,
                                               0,
                                               iv,
                                               12,
                                               add,
                                               (info[5] + asso->akey[1] - 5 - 12),
                                               &info[5 + (info[5] - 12 + 1)],
                                               12,
                                               &info[11],
                                               output);
                msg = &info[7];
                break;
            }
            case 0x20:
            {
                //仅加密
                output = heap.dalloc(info[5] + 4);
                if(!output)
                {
                    ret = -1;
                    break;
                }
                
                add = heap.dalloc(4);
                if(!add)
                {
                    ret = -1;
                    break;
                }
                
                iv = heap.dalloc(12);
                if(!iv)
                {
                    ret = -1;
                    break;
                }
                heap.copy(iv, &asso->callingtitle[2], 8);
                heap.copy(&iv[8], &info[7], 4);
                
                if(ret)
                {
                    break;
                }
                
                ret = mbedtls_gcm_auth_decrypt(&ctx,
                                               (info[5] - 5),
                                               iv,
                                               12,
                                               add,
                                               0,
                                               &info[5 + info[5]],
                                               0,
                                               &info[11],
                                               (output + 4));
                msg = output;
                break;
            }
            case 0x30:
            {
                //加密加认证
                output = heap.dalloc(info[5] + 4);
                if(!output)
                {
                    ret = -1;
                    break;
                }
                
                add = heap.dalloc(1 + asso->akey[1]);
                if(!add)
                {
                    ret = -1;
                    break;
                }
                add[0] = asso->info.sc;
                heap.copy(&add[1], &asso->akey[2], asso->akey[1]);
                
                iv = heap.dalloc(12);
                if(!iv)
                {
                    ret = -1;
                    break;
                }
                heap.copy(iv, &asso->callingtitle[2], 8);
                heap.copy(&iv[8], &info[7], 4);
                
                if(ret)
                {
                    break;
                }
                
                ret = mbedtls_gcm_auth_decrypt(&ctx,
                                               (info[5] - 5 - 12),
                                               iv,
                                               12,
                                               add,
                                               (1 + asso->akey[1]),
                                               &info[5 + (info[5] - 12 + 1)],
                                               12,
                                               &info[11],
                                               (output + 4));
                msg = output;
                break;
            }
            case 0x31:
			case 0x32:
            {
                //加密加认证
                output = heap.dalloc(info[14] + 4);
                if(!output)
                {
                    ret = -1;
                    break;
                }
                
                add = heap.dalloc(1 + asso->akey[1]);
                if(!add)
                {
                    ret = -1;
                    break;
                }
                add[0] = asso->info.sc;
                heap.copy(&add[1], &asso->akey[2], asso->akey[1]);
                
                iv = heap.dalloc(12);
                if(!iv)
                {
                    ret = -1;
                    break;
                }
                heap.copy(iv, &asso->callingtitle[2], 8);
                heap.copy(&iv[8], &info[16], 4);
                
                if(ret)
                {
                    break;
                }
                
                ret = mbedtls_gcm_auth_decrypt(&ctx,
                                               (info[1] - 18 - 12),
                                               iv,
                                               12,
                                               add,
                                               (1 + asso->akey[1]),
                                               &info[2 + info[1] - 12],
                                               12,
                                               &info[20],
                                               (output + 4));
                msg = output;
                break;
			}
            default:
            {
                ret = -1;
            }
        }
        
        if(add)
        {
            heap.free(add);
        }
        if(iv)
        {
            heap.free(iv);
        }
        
        mbedtls_gcm_free( &ctx );
        
        if(ret)
        {
            heap.set(asso->ap.conformance, 0, sizeof(asso->ap.conformance));
            if(output)
            {
                heap.free(output);
            }
            return;
        }
    }
    
    //dedicated_key
    if(msg[5])
    {
        if(msg[6] > 32)
        {
            if(output)
            {
                heap.free(output);
            }
            return;
        }
        else
        {
            if(msg[6] < 32)
            {
                heap.copy(asso->info.dedkey, &msg[5], (msg[6] + 2));
            }
        }
        
        offset = 7 + msg[6];
    }
    else
    {
        offset = 6;
    }
    
    //response_allowed
    if(msg[offset])
    {
        asso->info.response = msg[offset + 1];
        offset += 2;
    }
    else
    {
        offset += 1;
    }
    
    //proposed_quality
    if(msg[offset])
    {
        asso->info.quality = msg[offset + 1];
        offset += 2;
    }
    else
    {
        offset += 1;
    }
    
    //proposed_version
    asso->info.version = msg[offset];
    
    offset += 1;
    
    //conformance_block
    for(cnt=0; cnt<3; cnt++)
    {
        if((asso->ap.conformance[cnt] & msg[offset+4+cnt]) != asso->ap.conformance[cnt])
        {
            heap.set(asso->ap.conformance, 0, sizeof(asso->ap.conformance));
        }
    }
    
    //client_max_pdu
    asso->info.max_pdu = msg[offset + 7];
    asso->info.max_pdu <<= 8;
    asso->info.max_pdu += msg[offset + 8];
    
    if(asso->info.max_pdu > DLMS_CONFIG_MAX_APDU)
    {
        asso->info.max_pdu = DLMS_CONFIG_MAX_APDU;
    }
    
    if(output)
    {
        heap.free(output);
    }
}

/**	
  * @brief 
  */
static void asso_keys_flush(void)
{
    uint8_t cnt;
    
    for(cnt=0; cnt<DLMS_CONFIG_MAX_ASSO; cnt++)
    {
        if(!asso_list[cnt])
        {
            continue;
        }
        
        if((asso_list[cnt]->status == ASSOCIATED) && (asso_list[cnt]->diagnose == SUCCESS_HLS) || \
            (asso_list[cnt]->status == ASSOCIATION_PENDING))
        {
            //加载 ekay
            DLMS_CONFIG_LOAD_EKEY(asso_list[cnt]->ekey);
            //加载 akey
            DLMS_CONFIG_LOAD_AKEY(asso_list[cnt]->akey);
			//加载 server signing private key
			DLMS_CONFIG_LOAD_SSKEY(asso_list[cnt]->ssprikey);
			//加载 client signing public key
			DLMS_CONFIG_LOAD_CSKEY(asso_list[cnt]->cspubkey);
        }
    }
}

/**	
  * @brief 
  */
static void asso_aarq_none(struct __dlms_association *asso,
                           const struct __aarq_request *request,
                           uint8_t *buffer,
                           uint16_t buffer_length,
                           uint16_t *filled_length)
{
    uint8_t version = 0;
    
    *(buffer + 0) = (uint8_t)AARE;
    *(buffer + 1) = 0x00;
    
    *filled_length = 2;
    
    //protocol-version
    if(request->protocol_version)
    {
        if((request->protocol_version[1] == 0x02)&&(request->protocol_version[2] == 0x02)&&(request->protocol_version[3] == 0x84))
        {
            *(buffer + *filled_length + 0) = 0x80;
            *(buffer + *filled_length + 1) = 2;
            *(buffer + *filled_length + 2) = 7;
            *(buffer + *filled_length + 3) = 0x80;
            
            *filled_length += 4;
        }
        else
        {
            asso->diagnose = FAILURE_NO_REASION;
            version = 0xff;
        }
    }
    
    //application-context-name
    //LN referencing, with no ciphering: 2, 16, 756, 5, 8, 1, 1
    //LN referencing, with ciphering: 2, 16, 756, 5, 8, 1, 3
    *(buffer + *filled_length + 0) = 0xA1;
    *(buffer + *filled_length + 1) = 0x09;
    *(buffer + *filled_length + 2) = 0x06;
    *(buffer + *filled_length + 3) = 0x07;
    heap.copy((buffer + *filled_length + 4), "\x60\x85\x74\x05\x08\x01", 6);
    *(buffer + *filled_length + 10) = 0x01;
    if(asso->appl_name.id != 1)
    {
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    
    if(!request->application_context_name)
    {
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
	else if(memcmp(&request->application_context_name[4], (buffer + *filled_length + 4), 7) != 0)
    {
        encode_object_identifier((buffer + *filled_length + 4), &asso->appl_name);
        asso->diagnose = FAILURE_NO_REASION;
    }
    
    *filled_length += 11;
    
	if(asso->info.dedkey[0])
    {
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    else if(asso->info.version < 6)
    {
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    else if(asso->info.max_pdu <= 0x0b)
    {
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    
	if(asso->diagnose == SUCCESS_NOSEC_LLS)
	{
		asso->status = ASSOCIATED;
        asso->level = DLMS_ACCESS_LOWEST;
	}
    
    //association-result
    *(buffer + *filled_length + 0) = 0xA2;
    *(buffer + *filled_length + 1) = 0x03;
    *(buffer + *filled_length + 2) = 0x02;
    *(buffer + *filled_length + 3) = 0x01;
    if(asso->status == ASSOCIATED)
    {
        *(buffer + *filled_length + 4) = 0;
    }
    else
    {
        *(buffer + *filled_length + 4) = 1;
    }
    
    *filled_length += 5;
    
    //associate-source-diagnostic
    *(buffer + *filled_length + 0) = 0xA3;
    *(buffer + *filled_length + 1) = 0x05;
    if(version)
    {
        asso->diagnose = FAILURE_NO_REASION;
    }
    if((asso->diagnose == FAILURE_NO_REASION) && (version))
    {
        *(buffer + *filled_length + 2) = 0xA2;
    }
    else
    {
        *(buffer + *filled_length + 2) = 0xA1;
    }
    *(buffer + *filled_length + 3) = 0x03;
    *(buffer + *filled_length + 4) = 0x02;
    *(buffer + *filled_length + 5) = 0x01;
    *(buffer + *filled_length + 6) = (uint8_t)asso->diagnose;
    
    *filled_length += 7;
    
    //user-information
    *(buffer + *filled_length + 0) = 0xBE;
    *(buffer + *filled_length + 1) = 0x06;
    *(buffer + *filled_length + 2) = 0x04;
    *(buffer + *filled_length + 3) = 0x04;
    *(buffer + *filled_length + 4) = 0x0E;
    *(buffer + *filled_length + 5) = 0x01;
    
    //negotiated-dlms-version-number
    *(buffer + *filled_length + 6) = 0x06;
    
    if(asso->info.version < 6)
    {
        *(buffer + *filled_length + 7) = 0x01;
        *filled_length += 8;
    }
    else if((asso->ap.conformance[0] == 0) && (asso->ap.conformance[1] == 0) && (asso->ap.conformance[2] == 0))
    {
        *(buffer + *filled_length + 7) = 0x02;
        *filled_length += 8;
    }
    else if(asso->info.max_pdu <= 0x0b)
    {
        *(buffer + *filled_length + 7) = 0x03;
        *filled_length += 8;
    }
    else if((asso->diagnose == FAILURE_CONTEXT_NAME) || (asso->diagnose == FAILURE_NO_REASION))
    {
        *(buffer + *filled_length + 7) = 0x00;
        *filled_length += 8;
        return;
    }
    else if(asso->info.sc)
    {
        *(buffer + *filled_length + 7) = 0x00;
        *filled_length += 8;
    }
    else
    {
        //user-information
        *(buffer + *filled_length + 0) = 0xBE;
        *(buffer + *filled_length + 1) = 0x10;
        *(buffer + *filled_length + 2) = 0x04;
        *(buffer + *filled_length + 3) = 0x0E;
        
        *(buffer + *filled_length + 4) = 0x08;
        *(buffer + *filled_length + 5) = 0x00;
        //negotiated-dlms-version-number
        *(buffer + *filled_length + 6) = 0x06;
        
        *(buffer + *filled_length + 7) = 0x5F;
        *(buffer + *filled_length + 8) = 0x1F;
        
        //negotiated-conformance
        *(buffer + *filled_length + 9) = 0x04;
        *(buffer + *filled_length + 10) = 0x00;
        //ref "Green_Book_8th_edition" page 265
        *(buffer + *filled_length + 11) = asso->ap.conformance[0];
        *(buffer + *filled_length + 12) = asso->ap.conformance[1];
        *(buffer + *filled_length + 13) = asso->ap.conformance[2];
        
        //server-max-receive-pdu-size
        *(buffer + *filled_length + 14) = (uint8_t)(asso->info.max_pdu >> 8);
        *(buffer + *filled_length + 15) = (uint8_t)(asso->info.max_pdu);
        
        //value=0x0007 for LN and 0xFA00 for SN referencing
        *(buffer + *filled_length + 16) = 0x00;
        *(buffer + *filled_length + 17) = 0x07;
        
        *filled_length += 18;
    }
    
    //添加length
    *(buffer + 1) = *filled_length - 2;
}

/**	
  * @brief 
  */
static void asso_aarq_low(struct __dlms_association *asso,
                          const struct __aarq_request *request,
                          uint8_t *buffer,
                          uint16_t buffer_length,
                          uint16_t *filled_length)
{
    mbedtls_gcm_context ctx;
    int ret;
    uint8_t iv[12];
    uint8_t *input = (void *)0;
    uint8_t input_length;
    uint8_t version = 0;
    
    *(buffer + 0) = (uint8_t)AARE;
    *(buffer + 1) = 0x00;
    
    *filled_length = 2;
    
    //protocol-version
    if(request->protocol_version)
    {
        if((request->protocol_version[1] == 0x02)&&(request->protocol_version[2] == 0x02)&&(request->protocol_version[3] == 0x84))
        {
            *(buffer + *filled_length + 0) = 0x80;
            *(buffer + *filled_length + 1) = 2;
            *(buffer + *filled_length + 2) = 7;
            *(buffer + *filled_length + 3) = 0x80;
            
            *filled_length += 4;
        }
        else
        {
            asso->diagnose = FAILURE_NO_REASION;
            version = 0xff;
        }
    }
    
    //mechanism_name
    if(!request->mechanism_name)
    {
        //拒绝建立链接
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    else
    {
        if(memcmp(&request->mechanism_name[2], "\x60\x85\x74\x05\x08\x02", 6) != 0)
        {
            asso->diagnose = FAILURE_CONTEXT_NAME;
        }
        else if(asso->mech_name.id != 1)
        {
            asso->diagnose = FAILURE_CONTEXT_NAME;
        }
    }
    
    //sender_acse_requirements
    //LLS或者HLS，认证标识必须置位
    if(!request->sender_acse_requirements)
    {
        //拒绝建立链接
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    else
    {
        if(!(*(request->sender_acse_requirements + 3) & 0x80))
        {
            //拒绝建立链接
            asso->diagnose = FAILURE_CONTEXT_NAME;
        }
    }
    
    //calling_authentication_value
    //LLS时为密码
    if(!request->calling_authentication_value)
    {
        //拒绝建立链接
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    else
    {
        if(request->calling_authentication_value[1] > (32+2))
        {
            //拒绝建立链接
            asso->diagnose = FAILURE_CONTEXT_NAME;
        }
        else
        {
            if(asso->ap.ld == 0x3ffc)
            {
                DLMS_CONFIG_LOAD_PASSWD_MANAGE(asso->akey);
            }
            else
            {
                DLMS_CONFIG_LOAD_PASSWD(asso->akey);
            }
            
            if((memcmp(&asso->akey[2], &request->calling_authentication_value[4], asso->akey[1]) != 0) ||
			   (asso->akey[1] != request->calling_authentication_value[3]))
            {
                //拒绝建立链接
                asso->diagnose = FAILURE_CONTEXT_NAME;
            }
        }
    }
    
    //application-context-name
    //LN referencing, with no ciphering: 2, 16, 756, 5, 8, 1, 1
    //LN referencing, with ciphering: 2, 16, 756, 5, 8, 1, 3
    *(buffer + *filled_length + 0) = 0xA1;
    *(buffer + *filled_length + 1) = 0x09;
    *(buffer + *filled_length + 2) = 0x06;
    *(buffer + *filled_length + 3) = 0x07;
    heap.copy((buffer + *filled_length + 4), "\x60\x85\x74\x05\x08\x01", 6);
    *(buffer + *filled_length + 10) = asso->appl_name.id;
    if((asso->appl_name.id != 1) && (asso->appl_name.id != 3))
    {
        *(buffer + *filled_length + 10) = 0x01;
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    
    if(!request->application_context_name)
    {
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    else if(memcmp(&request->application_context_name[4], (buffer + *filled_length + 4), 6) != 0)
    {
        encode_object_identifier((buffer + *filled_length + 4), &asso->appl_name);
        asso->diagnose = FAILURE_NO_REASION;
    }
    
    *filled_length += 11;
    
    if(asso->info.dedkey[0])
    {
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
	else if(asso->info.version < 6)
    {
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    else if(asso->info.max_pdu <= 0x0b)
    {
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
	
	if(asso->diagnose == SUCCESS_NOSEC_LLS)
	{
		asso->status = ASSOCIATED;
        asso->level = DLMS_ACCESS_LOW;
	}
    
    //association-result
    *(buffer + *filled_length + 0) = 0xA2;
    *(buffer + *filled_length + 1) = 0x03;
    *(buffer + *filled_length + 2) = 0x02;
    *(buffer + *filled_length + 3) = 0x01;
    if(asso->status == ASSOCIATED)
    {
        *(buffer + *filled_length + 4) = 0;
    }
    else
    {
        *(buffer + *filled_length + 4) = 1;
    }
    
    *filled_length += 5;
    
    //associate-source-diagnostic
    *(buffer + *filled_length + 0) = 0xA3;
    *(buffer + *filled_length + 1) = 0x05;
    if(version)
    {
        asso->diagnose = FAILURE_NO_REASION;
    }
    if((asso->diagnose == FAILURE_NO_REASION) && (version))
    {
        *(buffer + *filled_length + 2) = 0xA2;
    }
    else
    {
        *(buffer + *filled_length + 2) = 0xA1;
    }
    *(buffer + *filled_length + 3) = 0x03;
    *(buffer + *filled_length + 4) = 0x02;
    *(buffer + *filled_length + 5) = 0x01;
    *(buffer + *filled_length + 6) = (uint8_t)asso->diagnose;
    
    *filled_length += 7;
    
    //AP-invocation-identifier called
    if(request->calling_AP_title)
    {
        *(buffer + *filled_length + 0) = 0xA4;
        *(buffer + *filled_length + 1) = 0x0A;
        *(buffer + *filled_length + 2) = 0x04;
        *(buffer + *filled_length + 3) = 0x08;
        dlms_asso_localtitle(buffer + *filled_length + 4);
        *filled_length += 12;
    }
    
    //user-information
    input = heap.dalloc(16);
    
    *(input + 0) = 0x0E;
    *(input + 1) = 0x01;
    //negotiated-dlms-version-number
    *(input + 2) = 0x06;
    
    if(asso->info.version < 6)
    {
        *(input + 3) = 0x01;
        input_length = 4;
    }
    else if((asso->ap.conformance[0] == 0) && (asso->ap.conformance[1] == 0) && (asso->ap.conformance[2] == 0))
    {
        *(input + 3) = 0x02;
        input_length = 4;
    }
    else if(asso->info.max_pdu <= 0x0b)
    {
        *(input + 3) = 0x03;
        input_length = 4;
    }
    else if((asso->diagnose == FAILURE_CONTEXT_NAME) || (asso->diagnose == FAILURE_NO_REASION))
    {
        *(input + 3) = 0x00;
        input_length = 4;
    }
    else
    {
        //user-information
        *(input + 0) = 0x08;
        *(input + 1) = 0x00;
        //negotiated-dlms-version-number
        *(input + 2) = 0x06;
        
        *(input + 3) = 0x5F;
        *(input + 4) = 0x1F;
        
        //negotiated-conformance
        *(input + 5) = 0x04;
        *(input + 6) = 0x00;
        //ref "Green_Book_8th_edition" page 265
        *(input + 7) = asso->ap.conformance[0];
        *(input + 8) = asso->ap.conformance[1];
        *(input + 9) = asso->ap.conformance[2];
        
        //server-max-receive-pdu-size
        *(input + 10) = (uint8_t)(asso->info.max_pdu >> 8);
        *(input + 11) = (uint8_t)(asso->info.max_pdu);
        
        //value=0x0007 for LN and 0xFA00 for SN referencing
        *(input + 12) = 0x00;
        *(input + 13) = 0x07;
        input_length = 14;
    }
    
    if(asso->info.sc == 0x20)
    {
        //加密 user-information
        dlms_asso_localtitle(iv);
        dlms_asso_fc(&iv[8]);
        
        *(buffer + *filled_length + 0) = 0xBE;
        *(buffer + *filled_length + 1) = input_length + 5 + 2 + 2;
        *(buffer + *filled_length + 2) = 0x04;
        *(buffer + *filled_length + 3) = input_length + 5 + 2;
        *(buffer + *filled_length + 4) = 0x28;
        *(buffer + *filled_length + 5) = input_length + 5;
        *(buffer + *filled_length + 6) = asso->info.sc;
        heap.copy((buffer + *filled_length + 7), &iv[8], 4);
        
        mbedtls_gcm_init(&ctx);
        
        ret = mbedtls_gcm_setkey(&ctx,
                                 MBEDTLS_CIPHER_ID_AES,
                                 &asso->ekey[2],
                                 asso->ekey[1]*8);
        
        if(ret != 0)
                goto enc_faild;
        
        ret = mbedtls_gcm_crypt_and_tag(&ctx,
                                        MBEDTLS_GCM_ENCRYPT,
                                        input_length,
                                        iv,
                                        sizeof(iv),
                                        (void *)0,
                                        0,
                                        input,
                                        (buffer + *filled_length + 8),
                                        0,
                                        (void *)0);
        if(ret != 0)
                goto enc_faild;
        
        mbedtls_gcm_free(&ctx);
        
        *filled_length += 11;
        *filled_length += input_length;
        *(buffer + 1) = *filled_length - 2;
    }
    else if(asso->info.sc == 0)
    {
        *(buffer + *filled_length + 0) = 0xBE;
        *(buffer + *filled_length + 1) = input_length + 2;
        *(buffer + *filled_length + 2) = 0x04;
        *(buffer + *filled_length + 3) = input_length;
        heap.copy((buffer + *filled_length + 4), input, input_length);
        *filled_length += 4;
        *filled_length += input_length;
        //添加length
        *(buffer + 1) = *filled_length - 2;
    }
    else
    {
        goto enc_faild;
    }
    
    heap.free(input);
    return;
    
enc_faild:
    mbedtls_gcm_free(&ctx);
    heap.free(input);
    *(buffer + *filled_length + 0) = 0xBE;
    *(buffer + *filled_length + 1) = 0x06;
    *(buffer + *filled_length + 2) = 0x04;
    *(buffer + *filled_length + 3) = 0x04;
    *(buffer + *filled_length + 4) = 0x0E;
    *(buffer + *filled_length + 5) = 0x01;
    *(buffer + *filled_length + 6) = 0x06;
    *(buffer + *filled_length + 7) = 0x00;
    *filled_length += 8;
    *(buffer + 1) = *filled_length - 2;
}

/**	
  * @brief 
  */
static void asso_aarq_high(struct __dlms_association *asso,
                           const struct __aarq_request *request,
                           uint8_t *buffer,
                           uint16_t buffer_length,
                           uint16_t *filled_length)
{
    mbedtls_gcm_context ctx;
    int ret;
    uint8_t iv[12];
    uint8_t tag[12];
    uint8_t *input = (void *)0;
    uint8_t *add = (void *)0;
    uint8_t input_length;
    uint8_t fail_version = 0;
    
    *(buffer + 0) = (uint8_t)AARE;
    *(buffer + 1) = 0x00;
    
    *filled_length = 2;
    
    //protocol-version
    if(request->protocol_version)
    {
        if((request->protocol_version[1] == 0x02)&&(request->protocol_version[2] == 0x02)&&(request->protocol_version[3] == 0x84))
        {
            *(buffer + *filled_length + 0) = 0x80;
            *(buffer + *filled_length + 1) = 2;
            *(buffer + *filled_length + 2) = 7;
            *(buffer + *filled_length + 3) = 0x80;
            
            *filled_length += 4;
        }
        else
        {
            fail_version = 0xff;
        }
    }
    
    //mechanism_name
    if(!request->mechanism_name)
    {
        //拒绝建立链接
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    else
    {
        if(memcmp(&request->mechanism_name[2], "\x60\x85\x74\x05\x08\x02", 6) != 0)
        {
            asso->diagnose = FAILURE_CONTEXT_NAME;
        }
        else if(asso->mech_name.id != 5)
        {
            asso->diagnose = FAILURE_CONTEXT_NAME;
        }
    }
    
    //sender_acse_requirements
    //LLS或者HLS，认证标识必须置位
    if(!request->sender_acse_requirements)
    {
        //拒绝建立链接
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    else
    {
        if(!(*(request->sender_acse_requirements + 3) & 0x80))
        {
            //拒绝建立链接
            asso->diagnose = FAILURE_CONTEXT_NAME;
        }
    }
    
    //calling_authentication_value
    //HLS时为随机数
    if(!request->calling_authentication_value)
    {
        //拒绝建立链接
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    else
    {
        if(request->calling_authentication_value[1] > (64+2))
        {
            //拒绝建立链接
            asso->diagnose = FAILURE_CONTEXT_NAME;
        }
        else
        {
            heap.copy(asso->ctos, &request->calling_authentication_value[2], request->calling_authentication_value[1]);
        }
    }
    
    //application-context-name
    //LN referencing, with no ciphering: 2, 16, 756, 5, 8, 1, 1
    //LN referencing, with ciphering: 2, 16, 756, 5, 8, 1, 3
    *(buffer + *filled_length + 0) = 0xA1;
    *(buffer + *filled_length + 1) = 0x09;
    *(buffer + *filled_length + 2) = 0x06;
    *(buffer + *filled_length + 3) = 0x07;
    heap.copy((buffer + *filled_length + 4), "\x60\x85\x74\x05\x08\x01", 6);
    *(buffer + *filled_length + 10) = 0x03;
    if(asso->appl_name.id != 3)
    {
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    
    if(!request->application_context_name)
    {
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    else if(memcmp(&request->application_context_name[4], (buffer + *filled_length + 4), 6) != 0)
    {
        encode_object_identifier((buffer + *filled_length + 4), &asso->appl_name);
        asso->diagnose = FAILURE_NO_REASION;
    }
    
    *filled_length += 11;
    
    if(asso->info.version < 6)
    {
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    else if(asso->info.max_pdu <= 0x0b)
    {
        asso->diagnose = FAILURE_CONTEXT_NAME;
    }
    
	if(asso->diagnose == SUCCESS_NOSEC_LLS)
	{
		asso->diagnose = SUCCESS_HLS;
		asso->status = ASSOCIATION_PENDING;
        asso->level = DLMS_ACCESS_LOW;
	}
    
    //association-result
    *(buffer + *filled_length + 0) = 0xA2;
    *(buffer + *filled_length + 1) = 0x03;
    *(buffer + *filled_length + 2) = 0x02;
    *(buffer + *filled_length + 3) = 0x01;
    if(asso->status != NON_ASSOCIATED)
    {
        *(buffer + *filled_length + 4) = 0;
    }
    else
    {
        *(buffer + *filled_length + 4) = 1;
    }
    
    *filled_length += 5;
    
    //associate-source-diagnostic
    *(buffer + *filled_length + 0) = 0xA3;
    *(buffer + *filled_length + 1) = 0x05;
    if(fail_version)
    {
        asso->diagnose = FAILURE_NO_REASION;
    }
    if((asso->diagnose == FAILURE_NO_REASION) && (fail_version))
    {
        *(buffer + *filled_length + 2) = 0xA2;
    }
    else
    {
        *(buffer + *filled_length + 2) = 0xA1;
    }
    *(buffer + *filled_length + 3) = 0x03;
    *(buffer + *filled_length + 4) = 0x02;
    *(buffer + *filled_length + 5) = 0x01;
    *(buffer + *filled_length + 6) = (uint8_t)asso->diagnose;
    
    *filled_length += 7;
    
    
    //AP-invocation-identifier called
    if(request->calling_AP_title)
    {
        *(buffer + *filled_length + 0) = 0xA4;
        *(buffer + *filled_length + 1) = 0x0A;
        *(buffer + *filled_length + 2) = 0x04;
        *(buffer + *filled_length + 3) = 0x08;
        dlms_asso_localtitle(buffer + *filled_length + 4);
        *filled_length += 12;
    }
    
    if(asso->diagnose == SUCCESS_HLS)
    {
        //sender-acse-requirements (HLS)
        if(request->sender_acse_requirements)
        {
            if(*(request->sender_acse_requirements + 3) & 0x80)
            {
                //responder-acse-requirements
                *(buffer + *filled_length + 0) = 0x88;
                *(buffer + *filled_length + 1) = 0x02;
                *(buffer + *filled_length + 2) = 0x07;
                *(buffer + *filled_length + 3) = 0x80;
                *filled_length += 4;
                
                //mechanism-name
                *(buffer + *filled_length + 0) = 0x89;
                *(buffer + *filled_length + 1) = 0x07;
                decode_object_identifier(&asso->mech_name, (buffer + *filled_length + 2));
                
                *filled_length += 9;
                
                //responding-authentication-value
                //产生 StoC
                *(buffer + *filled_length + 0) = 0xAA;
                *(buffer + *filled_length + 1) = 0x12;
                *(buffer + *filled_length + 2) = 0x80;
                *(buffer + *filled_length + 3) = 0x10;
                dlms_asso_random(16, (buffer + *filled_length + 4));
                heap.copy(asso->stoc, (buffer + *filled_length + 2), (2 + 16));
                *filled_length += (4 + 16);
            }
        }
    }
    
    //user-information
    input = heap.dalloc(16);
    
    *(input + 0) = 0x0E;
    *(input + 1) = 0x01;
    //negotiated-dlms-version-number
    *(input + 2) = 0x06;
    
    if(asso->info.version < 6)
    {
        *(input + 3) = 0x01;
        input_length = 4;
    }
    else if((asso->ap.conformance[0] == 0) && (asso->ap.conformance[1] == 0) && (asso->ap.conformance[2] == 0))
    {
        *(input + 3) = 0x02;
        input_length = 4;
    }
    else if(asso->info.max_pdu <= 0x0b)
    {
        *(input + 3) = 0x03;
        input_length = 4;
    }
    else if((asso->diagnose == FAILURE_CONTEXT_NAME) || (asso->diagnose == FAILURE_NO_REASION))
    {
        *(input + 3) = 0x00;
        input_length = 4;
    }
    else
    {
        //user-information
        *(input + 0) = 0x08;
        *(input + 1) = 0x00;
        //negotiated-dlms-version-number
        *(input + 2) = 0x06;
        
        *(input + 3) = 0x5F;
        *(input + 4) = 0x1F;
        
        //negotiated-conformance
        *(input + 5) = 0x04;
        *(input + 6) = 0x00;
        //ref "Green_Book_8th_edition" page 265
        *(input + 7) = asso->ap.conformance[0];
        *(input + 8) = asso->ap.conformance[1];
        *(input + 9) = asso->ap.conformance[2];
        
        //server-max-receive-pdu-size
        *(input + 10) = (uint8_t)(asso->info.max_pdu >> 8);
        *(input + 11) = (uint8_t)(asso->info.max_pdu);
        
        //value=0x0007 for LN and 0xFA00 for SN referencing
        *(input + 12) = 0x00;
        *(input + 13) = 0x07;
        input_length = 14;
    }
    
    if(asso->info.sc== 0x10)
    {
        //仅认证 user-information
        dlms_asso_localtitle(iv);
        dlms_asso_fc(&iv[8]);
        
        *(buffer + *filled_length + 0) = 0xBE;
        *(buffer + *filled_length + 1) = input_length + 5 + 2 + 2 + sizeof(tag);
        *(buffer + *filled_length + 2) = 0x04;
        *(buffer + *filled_length + 3) = input_length + 5 + 2 + sizeof(tag);
        *(buffer + *filled_length + 4) = 0x28;
        *(buffer + *filled_length + 5) = input_length + 5 + sizeof(tag);
        *(buffer + *filled_length + 6) = asso->info.sc;
        heap.copy((buffer + *filled_length + 7), &iv[8], 4);
        heap.copy((buffer + *filled_length + 11), input, input_length);
        
        mbedtls_gcm_init(&ctx);
        
        ret = mbedtls_gcm_setkey(&ctx,
                                 MBEDTLS_CIPHER_ID_AES,
                                 &asso->ekey[2],
                                 asso->ekey[1]*8);
        
        if(ret != 0)
                goto enc_faild;
        
        add = heap.dalloc(1 + asso->akey[1] + input_length);
        if(!add)
                goto enc_faild;
        add[0] = asso->info.sc;
        heap.copy(&add[1], &asso->akey[2], asso->akey[1]);
        heap.copy(&add[1+asso->akey[1]], input, input_length);
        
        ret = mbedtls_gcm_crypt_and_tag(&ctx,
                                        MBEDTLS_GCM_ENCRYPT,
                                        0,
                                        iv,
                                        sizeof(iv),
                                        add,
                                        (1 + asso->akey[1] + input_length),
                                        (void *)0,
                                        (void *)0,
                                        sizeof(tag),
                                        tag);
        heap.free(add);
        
        if(ret != 0)
                goto enc_faild;
        
        mbedtls_gcm_free(&ctx);
        
        heap.copy((buffer + *filled_length + 11 + input_length), tag, sizeof(tag));
        *filled_length += 11;
        *filled_length += input_length;
        *filled_length += sizeof(tag);
        *(buffer + 1) = *filled_length - 2;
    }
    else if(asso->info.sc == 0x20)
    {
        //仅加密 user-information
        dlms_asso_localtitle(iv);
        dlms_asso_fc(&iv[8]);
        
        *(buffer + *filled_length + 0) = 0xBE;
        *(buffer + *filled_length + 1) = input_length + 5 + 2 + 2;
        *(buffer + *filled_length + 2) = 0x04;
        *(buffer + *filled_length + 3) = input_length + 5 + 2;
        *(buffer + *filled_length + 4) = 0x28;
        *(buffer + *filled_length + 5) = input_length + 5;
        *(buffer + *filled_length + 6) = asso->info.sc;
        heap.copy((buffer + *filled_length + 7), &iv[8], 4);
        
        mbedtls_gcm_init(&ctx);
        
        ret = mbedtls_gcm_setkey(&ctx,
                                 MBEDTLS_CIPHER_ID_AES,
                                 &asso->ekey[2],
                                 asso->ekey[1]*8);
        
        if(ret != 0)
                goto enc_faild;
        
        ret = mbedtls_gcm_crypt_and_tag(&ctx,
                                        MBEDTLS_GCM_ENCRYPT,
                                        input_length,
                                        iv,
                                        sizeof(iv),
                                        (void *)0,
                                        0,
                                        input,
                                        (buffer + *filled_length + 11),
                                        0,
                                        (void *)0);
        if(ret != 0)
                goto enc_faild;
        
        mbedtls_gcm_free(&ctx);
        
        *filled_length += 11;
        *filled_length += input_length;
        *(buffer + 1) = *filled_length - 2;
    }
    else if(asso->info.sc == 0x30)
    {
        //加密和认证 user-information
        dlms_asso_localtitle(iv);
        dlms_asso_fc(&iv[8]);
        
        *(buffer + *filled_length + 0) = 0xBE;
        *(buffer + *filled_length + 1) = input_length + 5 + 2 + 2 + sizeof(tag);
        *(buffer + *filled_length + 2) = 0x04;
        *(buffer + *filled_length + 3) = input_length + 5 + 2 + sizeof(tag);
        *(buffer + *filled_length + 4) = 0x28;
        *(buffer + *filled_length + 5) = input_length + 5 + sizeof(tag);
        *(buffer + *filled_length + 6) = asso->info.sc;
        heap.copy((buffer + *filled_length + 7), &iv[8], 4);
        
        mbedtls_gcm_init(&ctx);
        
        ret = mbedtls_gcm_setkey(&ctx,
                                 MBEDTLS_CIPHER_ID_AES,
                                 &asso->ekey[2],
                                 asso->ekey[1]*8);
        
        if(ret != 0)
                goto enc_faild;
        
        add = heap.dalloc(1 + asso->akey[1]);
        if(!add)
                goto enc_faild;
        add[0] = asso->info.sc;
        heap.copy(&add[1], &asso->akey[2], asso->akey[1]);
        
        ret = mbedtls_gcm_crypt_and_tag(&ctx,
                                        MBEDTLS_GCM_ENCRYPT,
                                        input_length,
                                        iv,
                                        sizeof(iv),
                                        add,
                                        (1 + asso->akey[1]),
                                        input,
                                        (buffer + *filled_length + 11),
                                        sizeof(tag),
                                        tag);
        heap.free(add);
        
        if(ret != 0)
                goto enc_faild;
        
        mbedtls_gcm_free(&ctx);
        
        heap.copy((buffer + *filled_length + 11 + input_length), tag, sizeof(tag));
        *filled_length += 11;
        *filled_length += input_length;
        *filled_length += sizeof(tag);
        *(buffer + 1) = *filled_length - 2;
    }
    else if((asso->info.sc == 0x31) || (asso->info.sc == 0x32))
    {
        //加密和认证 user-information
        dlms_asso_localtitle(iv);
        dlms_asso_fc(&iv[8]);
        
        *(buffer + *filled_length + 0) = 0xBE;
        *(buffer + *filled_length + 1) = input_length + 2 + 10 + 6 + sizeof(tag);
        *(buffer + *filled_length + 2) = 0x04;
        *(buffer + *filled_length + 3) = input_length + 10 + 6 + sizeof(tag);
        *(buffer + *filled_length + 4) = 0xDB;
        *(buffer + *filled_length + 5) = 0x08;
		heap.copy((buffer + *filled_length + 6), iv, 8);
        *(buffer + *filled_length + 14) = input_length + 5 + sizeof(tag);
		*(buffer + *filled_length + 15) = asso->info.sc;
		heap.copy((buffer + *filled_length + 16), &iv[8], 4);
        
        mbedtls_gcm_init(&ctx);
        
        ret = mbedtls_gcm_setkey(&ctx,
                                 MBEDTLS_CIPHER_ID_AES,
                                 &asso->ekey[2],
                                 asso->ekey[1]*8);
        
        if(ret != 0)
                goto enc_faild;
        
        add = heap.dalloc(1 + asso->akey[1]);
        if(!add)
                goto enc_faild;
        add[0] = asso->info.sc;
        heap.copy(&add[1], &asso->akey[2], asso->akey[1]);
        
        ret = mbedtls_gcm_crypt_and_tag(&ctx,
                                        MBEDTLS_GCM_ENCRYPT,
                                        input_length,
                                        iv,
                                        sizeof(iv),
                                        add,
                                        (1 + asso->akey[1]),
                                        input,
                                        (buffer + *filled_length + 20),
                                        sizeof(tag),
                                        tag);
        heap.free(add);
        
        if(ret != 0)
                goto enc_faild;
        
        mbedtls_gcm_free(&ctx);
        
        heap.copy((buffer + *filled_length + 20 + input_length), tag, sizeof(tag));
        *filled_length += 20;
        *filled_length += input_length;
        *filled_length += sizeof(tag);
        *(buffer + 1) = *filled_length - 2;
    }
    else
    {
        *(buffer + *filled_length + 0) = 0xBE;
        *(buffer + *filled_length + 1) = input_length + 2;
        *(buffer + *filled_length + 2) = 0x04;
        *(buffer + *filled_length + 3) = input_length;
        heap.copy((buffer + *filled_length + 4), input, input_length);
        *filled_length += 4;
        *filled_length += input_length;
        //添加length
        *(buffer + 1) = *filled_length - 2;
    }
    
    heap.free(input);
    return;
    
enc_faild:
    mbedtls_gcm_free(&ctx);
    heap.free(input);
    *(buffer + *filled_length + 0) = 0xBE;
    *(buffer + *filled_length + 1) = 0x06;
    *(buffer + *filled_length + 2) = 0x04;
    *(buffer + *filled_length + 3) = 0x04;
    *(buffer + *filled_length + 4) = 0x0E;
    *(buffer + *filled_length + 5) = 0x01;
    *(buffer + *filled_length + 6) = 0x06;
    *(buffer + *filled_length + 7) = 0x00;
    *filled_length += 8;
    *(buffer + 1) = *filled_length - 2;
}


/**	
  * @brief 
  */
static void asso_aarq(struct __dlms_association *asso,
                      const uint8_t *info,
                      uint16_t length,
                      uint8_t *buffer,
                      uint16_t buffer_length,
                      uint16_t *filled_length)
{
    struct __aarq_request request;
    
    parse_aarq_frame(info, length, &request);
    
    //application-context-name 和 user_information 是建立连接所必须的
    
    //保存 application-context-name
    //2 16 756 5 8 1 x
    //context_id = 1 : Logical_Name_Referencing_No_Ciphering 只响应不加密报文
    //context_id = 3 : Logical_Name_Referencing_With_Ciphering 响应加密报文和不加密报文
    if(request.application_context_name)
    {
        encode_object_identifier((request.application_context_name + 4), &asso->appl_name);
    }
    
    //保存 calling_AP_title
    if(request.calling_AP_title)
    {
    	//保存 calling_AP_title
        if(request.calling_AP_title[1] > 10)
        {
            heap.copy(asso->callingtitle, &request.calling_AP_title[2], 10);
        }
        else
        {
        	heap.copy(asso->callingtitle, &request.calling_AP_title[2], request.calling_AP_title[1]);
		}
		
        if(asso->callingtitle[1] > 0x08)
        {
        	asso->callingtitle[1] = 0x08;
		}
    }
    
    //保存 user_information
    if(request.user_information)
    {
        parse_user_information(request.user_information, asso);
    }
    
    //保存 mechanism_name
    //2 16 756 5 8 2 x
    //x=0 最低等级连接，不需要认证，此时 mechanism_name 可以没有
    //x=1 低等级连接，使用密码来认证
    //x=2 高等级连接，使用自定义加密算法来认证
    //x=3 高等级连接，使用MD5加密算法来认证
    //x=4 高等级连接，使用SHA-1加密算法来认证
    //x=5 高等级连接，使用GMAC加密算法来认证
    //x=6 高等级连接，使用SHA-256加密算法来认证
    //x=7 高等级连接，使用_ECDSA加密算法来认证
    //目前支持 0  1  5
    if(request.mechanism_name)
    {
        encode_object_identifier((request.mechanism_name + 2), &asso->mech_name);
    }
    
    //通过 mech_name id 来判别要建立什么级别的连接
    switch(asso->mech_name.id)
    {
        case 0:
        {
            //无认证
            asso_aarq_none(asso, &request, buffer, buffer_length, filled_length);
            break;
        }
        case 1:
        {
            //低级别认证
            asso_aarq_low(asso, &request, buffer, buffer_length, filled_length);
            break;
        }
        default:
        {
            //高级别认证
            asso_aarq_high(asso, &request, buffer, buffer_length, filled_length);
            break;
        }
    }
}

/**	
  * @brief 
  */
static void asso_rlrq(struct __dlms_association *asso,
                      const uint8_t *info,
                      uint16_t length,
                      uint8_t *buffer,
                      uint16_t buffer_length,
                      uint16_t *filled_length)
{
    uint8_t cnt;
    
    for(cnt=0; cnt<DLMS_CONFIG_MAX_ASSO; cnt++)
    {
        if(!asso_list[cnt])
        {
            continue;
        }
        
        if(asso_list[cnt] == asso)
        {
            if(asso_list[cnt]->appl)
            {
                heap.free(asso_list[cnt]->appl);
            }
            heap.free(asso_list[cnt]);
            asso_list[cnt] = (void *)0;
        }
    }
    
    if(buffer_length < 3)
    {
        return;
    }
    
    buffer[0] = RLRE;
    buffer[1] = 0x00;
    buffer[2] = 0x00;
    *filled_length = 3;
}

/**	
  * @brief 
  */
static void asso_request(const uint8_t *info,
                         uint16_t length,
                         uint8_t *buffer,
                         uint16_t buffer_length,
                         uint16_t *filled_length)
{
    DLMS_CONFIG_COSEM_REQUEST(info, length, buffer, buffer_length, filled_length);
}









/**	
  * @brief 
  */
void dlms_asso_gateway(struct __dlms_session session,
                       const uint8_t *info,
                       uint16_t length,
                       uint8_t *buffer,
                       uint16_t buffer_length,
                       uint16_t *filled_length)
{
    uint8_t cnt;
    const struct __ap *ap_support = (const struct __ap *)0;
    
    //有效性判断
    if((!info) || (!length) || (!buffer) || (!buffer_length) || (!filled_length))
    {
        return;
    }
    
    //清零返回报文长度
    *filled_length = 0;
    //清零当前连接指针
    asso_current = (void *)0;
    
    //查询AP是否已完成协商
    for(cnt=0; cnt<DLMS_CONFIG_MAX_ASSO; cnt++)
    {
        if(!asso_list[cnt])
        {
            continue;
        }
        
        if((asso_list[cnt]->ap.ld == session.sap) && (asso_list[cnt]->session == session.session))
        {
            asso_current = asso_list[cnt];
        }
    }
    
    //解析报文
    switch(*info)
    {
        case AARQ:
        {
		    //查询SAP是否在支持的SAP列表中
		    for(cnt=0; cnt<DLMS_AP_AMOUNT; cnt++)
		    {
		        if(ap_support_list[cnt].ld == session.sap)
		        {
		            ap_support = &ap_support_list[cnt];
		            break;
		        }
		    }
		    
		    //没有查询到支持的AP
		    if(!ap_support)
		    {
		        return;
		    }
		    
		    //AP不在已完成的协商列表中，生成一个新的AP对象
		    if(!asso_current)
		    {
			    //查询一个未被占用的节点
		        for(cnt=0; cnt<DLMS_CONFIG_MAX_ASSO; cnt++)
		        {
		            if(!asso_list[cnt])
		            {
		                asso_list[cnt] = heap.salloc(NAME_PROTOCOL, sizeof(struct __dlms_association));
		                if(!asso_list[cnt])
		                {
		                    return;
		                }
		                
		                asso_current = asso_list[cnt];
		                heap.set(asso_current, 0, sizeof(struct __dlms_association));
		                break;
		            }
		        }
		        
		        //AP对象生成失败
		        if(!asso_current)
		        {
		            return;
		        }
		    }
        	
		    //清理连接状态
		    if(asso_current->appl)
		    {
		        heap.free(asso_current->appl);
		    }
    		heap.set(asso_current, 0, sizeof(struct __dlms_association));
    		heap.copy(&asso_current->ap, ap_support, sizeof(struct __ap));
    		asso_current->session = session.session;
    		
	        //初始化FC
	        srand((unsigned int)jiffy.value());
	        asso_current->fc = (uint32_t)rand();
    		
            asso_aarq(asso_current, info, length, buffer, buffer_length, filled_length);
            break;
        }
        case RLRQ:
        {
        	if(!asso_current)
        	{
        		return;
			}
            asso_rlrq(asso_current, info, length, buffer, buffer_length, filled_length);
            break;
        }
        default:
        {
        	if(!asso_current)
        	{
        		return;
			}
			
            if(asso_current->status != NON_ASSOCIATED)
            {
                asso_request(info, length, buffer, buffer_length, filled_length);
                
                if(key_is_eliminate)
                {
                    key_is_eliminate = 0;
                    asso_keys_flush();
                }
            }
            break;
        }
    }
    asso_current = (void *)0;
}

/**
  * @brief 清理 Association
  */
void dlms_asso_cleanup(struct __dlms_session session)
{
    uint8_t cnt;
    
    //查询AP是否已经在协商列表中
    for(cnt=0; cnt<DLMS_CONFIG_MAX_ASSO; cnt++)
    {
        if(!asso_list[cnt])
        {
            continue;
        }
        
        if((asso_list[cnt]->ap.ld == session.sap) && (asso_list[cnt]->session == session.session))
        {
            if(asso_list[cnt]->appl)
            {
                heap.free(asso_list[cnt]->appl);
            }
            heap.free(asso_list[cnt]);
            asso_list[cnt] = (void *)0;
        }
    }
}

/**
  * @brief 获取 client AP
  */
uint16_t dlms_asso_session(void)
{
    if(!asso_current)
    {
        return(0);
    }
	
	return(asso_current->session);
}

/**
  * @brief 获取 Association Status
  */
enum __asso_status dlms_asso_status(void)
{
    if(!asso_current)
    {
        return(NON_ASSOCIATED);
    }
    
    return(asso_current->status);
}

/**	
  * @brief 应用层最大报文传输长度
  */
uint16_t dlms_asso_mtu(void)
{
    if(DLMS_CONFIG_MAX_APDU < 32)
    {
        return(32);
    }
    
    return(DLMS_CONFIG_MAX_APDU);
}

/**
  * @brief 获取 Calling AP Title 8字节
  */
uint8_t dlms_asso_callingtitle(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    if(asso_current->callingtitle[1] >= 8)
    {
        heap.copy(buffer, &asso_current->callingtitle[2], 8);
        return(8);
    }
    else
    {
        heap.copy(buffer, &asso_current->callingtitle[2], asso_current->callingtitle[1]);
        return(asso_current->callingtitle[1]);
    }
}

/**
  * @brief 修改 Calling AP Title 8字节
  */
uint8_t dlms_asso_modify_callingtitle(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    asso_current->callingtitle[1] = 8;
    heap.copy(&asso_current->callingtitle[2], buffer, 8);
    
    return(8);
}

/**
  * @brief 获取 Local AP Title 8字节
  */
uint8_t dlms_asso_localtitle(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    heap.set(buffer, 0, 8);
    
    if(asso_current->localtitle[1] != 8)
    {
        return(0);
    }
    
    heap.copy(buffer, &asso_current->localtitle[2], 8);
    
    return(8);
}

/**
  * @brief 获取 stoc <64字节
  */
uint8_t dlms_asso_stoc(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    if((asso_current->stoc[1] > 64) || (asso_current->stoc[1] == 0))
    {
        return(0);
    }
    
    heap.copy(buffer, &asso_current->stoc[2], asso_current->stoc[1]);
    
    return(asso_current->stoc[1]);
}

/**
  * @brief 获取 ctos <64字节
  */
uint8_t dlms_asso_ctos(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    if((asso_current->ctos[1] > 64) || (asso_current->ctos[1] == 0))
    {
        return(0);
    }
    
    heap.copy(buffer, &asso_current->ctos[2], asso_current->ctos[1]);
    
    return(asso_current->ctos[1]);
}

/**
  * @brief 接受 F(CtoS)
  */
uint8_t dlms_asso_accept_fctos(void)
{
    if(!asso_current)
    {
        return(0);
    }
    
    if(asso_current->status == ASSOCIATION_PENDING)
    {
        asso_current->status = ASSOCIATED;
        asso_current->level = DLMS_ACCESS_HIGH;
        return(0xff);
    }
    
    return(0);
}

/**
  * @brief 获取 Security Control Byte 1字节
  */
uint8_t dlms_asso_sc(void)
{
    if(!asso_current)
    {
        return(0);
    }
    
    return(asso_current->sc);
}

/**
  * @brief 获取 Frame Counter 4 字节
  */
uint8_t dlms_asso_fc(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    asso_current->fc += 1;
    
    buffer[0] = (uint8_t)(asso_current->fc >> 24);
    buffer[1] = (uint8_t)(asso_current->fc >> 16);
    buffer[2] = (uint8_t)(asso_current->fc >> 8);
    buffer[3] = (uint8_t)(asso_current->fc >> 0);
    
    return(4);
}

/**
  * @brief 获取 akey <=32字节
  */
uint8_t dlms_asso_akey(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    if((asso_current->akey[1] > 32) || (asso_current->akey[1] == 0))
    {
        return(0);
    }
    
    heap.copy(buffer, &asso_current->akey[2], asso_current->akey[1]);
    
    return(asso_current->akey[1]);
}

/**
  * @brief 获取 ekey <=32字节
  */
uint8_t dlms_asso_ekey(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    if((asso_current->ekey[1] > 32) || (asso_current->ekey[1] == 0))
    {
        return(0);
    }
    
    heap.copy(buffer, &asso_current->ekey[2], asso_current->ekey[1]);
    
    return(asso_current->ekey[1]);
}

/**
  * @brief 获取 dedkey <=32字节
  */
uint8_t dlms_asso_dedkey(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    if((asso_current->info.dedkey[1] > 32) || (asso_current->info.dedkey[1] == 0))
    {
        return(0);
    }
    
    heap.copy(buffer, &asso_current->info.dedkey[2], asso_current->info.dedkey[1]);
    
    return(asso_current->info.dedkey[1]);
}

/**
  * @brief 获取 server signing private key <=48字节
  */
uint8_t dlms_asso_ssprikey(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    if((asso_current->ssprikey[1] > 48) || (asso_current->ssprikey[1] == 0))
    {
        return(0);
    }
    
    heap.copy(buffer, &asso_current->ssprikey[2], asso_current->ssprikey[1]);
    
    return(asso_current->ssprikey[1]);
}

/**
  * @brief 获取 client signing public key <=96字节
  */
uint8_t dlms_asso_cspubkey(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    if((asso_current->cspubkey[1] > 96) || (asso_current->cspubkey[1] == 0))
    {
        return(0);
    }
    
    heap.copy(buffer, &asso_current->cspubkey[2], asso_current->cspubkey[1]);
    
    return(asso_current->cspubkey[1]);
}

/**
  * @brief 密钥需要更新
  */
void dlms_asso_key_eliminate(void)
{
    key_is_eliminate = 0xff;
}

/**
  * @brief 获取 Application Context Name
  */
uint8_t dlms_asso_applname(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    heap.copy(buffer, (uint8_t *)&(asso_current->appl_name), sizeof(asso_current->appl_name));
    
    return(sizeof(asso_current->appl_name));
}

/**
  * @brief 获取 Authentication Mechanism Name
  */
uint8_t dlms_asso_mechname(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    heap.copy(buffer, (uint8_t *)&(asso_current->mech_name), sizeof(asso_current->mech_name));
    
    return(sizeof(asso_current->mech_name));
}

/**
  * @brief 获取 xDLMS Context Info
  */
uint8_t dlms_asso_contextinfo(uint8_t *buffer)
{
    if(!asso_current || !buffer)
    {
        return(0);
    }
    
    heap.copy(buffer, (uint8_t *)&(asso_current->info), sizeof(asso_current->info));
    
    return(sizeof(asso_current->info));
}

/**
  * @brief 获取 Access Level
  */
enum __dlms_access_level dlms_asso_level(void)
{
    if(!asso_current)
    {
        return(DLMS_ACCESS_NO);
    }
    
    return(asso_current->level);
}

/**
  * @brief 获取 object list suit
  */
uint8_t dlms_asso_suit(void)
{
    if(!asso_current)
    {
        return(0);
    }
    
    return(asso_current->ap.suit);
}

/**
  * @brief 获取 应用层 附属缓冲
  */
void * dlms_asso_storage(void)
{
    if(!asso_current)
    {
        return(0);
    }
    
    return(asso_current->appl);
}

uint16_t dlms_asso_storage_size(void)
{
    if(!asso_current)
    {
        return(0);
    }
    
    if(!asso_current->appl)
    {
        return(0);
    }
    
    return(asso_current->sz_appl);
}

/**
  * @brief 申请 应用层 附属缓冲
  */
void * dlms_asso_attach_storage(uint16_t size)
{
    if(!asso_current || !size)
    {
        return(0);
    }
    
    if(asso_current->appl)
    {
        heap.free(asso_current->appl);
    }
    
    asso_current->appl = heap.salloc(NAME_PROTOCOL, size);
    if(asso_current->appl)
    {
        heap.set(asso_current->appl, 0, size);
    }
    
    asso_current->sz_appl = size;
    
    return(asso_current->appl);
}

/**
  * @brief 获取 随机字符串
  */
void dlms_asso_random(uint8_t length, uint8_t *buffer)
{
    uint16_t val;
    uint8_t cnt;
    uint8_t loop = (length + 2) / 2;
    
    if(!length || !buffer)
    {
        return;
    }
    
    srand((unsigned int)jiffy.value());
    
    for(cnt=0; cnt<loop; cnt++)
    {
        val = (uint16_t)rand();
        
        if((cnt*2 + 0) >= length)
        {
            break;
        }
        buffer[cnt*2 + 0] = (((val >> 0) & 0xff) % 74) + 48;
        
        if((cnt*2 + 1) < length)
        {
            break;
        }
        buffer[cnt*2 + 1] = (((val >> 8) & 0xff) % 74) + 48;
    }
}
