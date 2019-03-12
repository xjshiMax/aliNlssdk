/* asr mod. Copyright 2017, xuhuaiyi ALL RIGHTS RESERVED!
    Author: cdevelop@qq.com(wwww.ddrj.com)

    之前写了一个商业模块mod_vad，以及二次开发接口SmartIVR(RESTful API)详细看 http://www.dingdingtong.cn/smartivr/
    很多用户只想测试一下ASR的效果,以及希望自己掌握获取FreeSWITCH语音数据的原理和过程，所以特意写了这个开源项目供大家参考。
    本项目的技术交流可以加群：340129771
    如果需要商业服务和支持可以联系QQ：1280791187，或者微信：cdevelop
    
    本项目是通过阿里云实时语音识别接口把通话中的语音流发送到阿里识别服务器，识别结果通过ESL事件通知业务程序。


*/

#include <switch.h>
#if defined(_WIN32)
#include <windows.h>
#include "pthread.h"
#else
#include <unistd.h>
#include <pthread.h>
#endif

#include <ctime>
#include <map>
#include <string>
#include <iostream>
#include <vector>
#include <fstream>
#include "nlsClient.h"
#include "nlsEvent.h"
#include "speechRecognizerRequest.h"

#include "nlsCommonSdk/Token.h"

#define FRAME_SIZE 3200
#define SAMPLE_RATE 16000
#define SAMPLE_RATE_8000 8000

using std::map;
using std::string;
using std::vector;
using std::cout;
using std::endl;
using std::ifstream;
using std::ios;

using namespace AlibabaNlsCommon;

using AlibabaNls::NlsClient;
using AlibabaNls::NlsEvent;
using AlibabaNls::LogDebug;
using AlibabaNls::LogInfo;
using AlibabaNls::SpeechRecognizerCallback;
using AlibabaNls::SpeechRecognizerRequest;

SWITCH_MODULE_LOAD_FUNCTION(mod_asr_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_asr_shutdown);

extern "C" {
	SWITCH_MODULE_DEFINITION(mod_asr, mod_asr_load, mod_asr_shutdown, NULL);
};


/**
* 全局维护一个服务鉴权token和其对应的有效期时间戳，
* 每次调用服务之前，首先判断token是否已经过期，
* 如果已经过期，则根据AccessKey ID和AccessKey Secret重新生成一个token，并更新这个全局的token和其有效期时间戳。
*
* 注意：不要每次调用服务之前都重新生成新token，只需在token即将过期时重新生成即可。所有的服务并发可共用一个token。
*/
string g_akId = "";
string g_akSecret = "";
string g_token = "";
long g_expireTime = -1;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {

    switch_core_session_t   *session;
    switch_media_bug_t      *bug;

    char                    *id;
    char                    *seceret;
	char					*appkey;
	char					*mainUUID;
    int                     stop;
	SpeechRecognizerCallback* callback;
	SpeechRecognizerRequest              *request;

} switch_da_t;

/**
* 根据AccessKey ID和AccessKey Secret重新生成一个token，并获取其有效期时间戳
*/
int generateToken(string akId, string akSecret, string* token, long* expireTime) {
	NlsToken nlsTokenRequest;
	nlsTokenRequest.setAccessKeyId(akId);
	nlsTokenRequest.setKeySecret(akSecret);

	if (-1 == nlsTokenRequest.applyNlsToken()) {
		cout << "Failed: " << nlsTokenRequest.getErrorMsg() << endl; /*获取失败原因*/

		return -1;
	}

	*token = nlsTokenRequest.getToken();
	*expireTime = nlsTokenRequest.getExpireTime();

	return 0;
}


/**
* @brief 调用start(), 成功与云端建立连接, sdk内部线程上报started事件
* @note 不允许在回调函数内部调用stop(), releaseRecognizerRequest()对象操作, 否则会异常
* @param cbEvent 回调事件结构, 详见nlsEvent.h
* @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
* @return
*/
void OnRecognitionStarted(NlsEvent* cbEvent, void* cbParam) {
	cout << "OnRecognitionStarted: "
		<< "status code: " << cbEvent->getStausCode()  // 获取消息的状态码，成功为0或者20000000，失败时对应失败的错误码
		<< ", task id: " << cbEvent->getTaskId()   // 当前任务的task id，方便定位问题，建议输出
		<< endl;
	// cout << "OnRecognitionStarted: All response:" << cbEvent->getAllResponse() << endl; // 获取服务端返回的全部信息
}

/**
* @brief 设置允许返回中间结果参数, sdk在接收到云端返回到中间结果时, sdk内部线程上报ResultChanged事件
* @note 不允许在回调函数内部调用stop(), releaseRecognizerRequest()对象操作, 否则会异常
* @param cbEvent 回调事件结构, 详见nlsEvent.h
* @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
* @return
*/
void OnRecognitionResultChanged(NlsEvent* cbEvent, void* cbParam) {
	
	cout << "OnRecognitionResultChanged: "
		<< "status code: " << cbEvent->getStausCode()  // 获取消息的状态码，成功为0或者20000000，失败时对应失败的错误码
		<< ", task id: " << cbEvent->getTaskId()    // 当前任务的task id，方便定位问题，建议输出
		<< ", result: " << cbEvent->getResult()     // 获取中间识别结果
		<< endl;
	// cout << "OnRecognitionResultChanged: All response:" << cbEvent->getAllResponse() << endl; // 获取服务端返回的全部信息
}

/**
* @brief sdk在接收到云端返回识别结束消息时, sdk内部线程上报Completed事件
* @note 上报Completed事件之后, SDK内部会关闭识别连接通道. 此时调用sendAudio会返回-1, 请停止发送.
*       不允许在回调函数内部调用stop(), releaseRecognizerRequest()对象操作, 否则会异常.
* @param cbEvent 回调事件结构, 详见nlsEvent.h
* @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
* @return
*/
void OnRecognitionCompleted(NlsEvent* cbEvent, void* cbParam) {
// 	ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
// 	cout << "CbParam: " << tmpParam->iExg << " " << tmpParam->sExg << endl; // 仅表示自定义参数示例
// 
 	cout << "OnRecognitionCompleted: "
 		<< "status code: " << cbEvent->getStausCode()  // 获取消息的状态码，成功为0或者20000000，失败时对应失败的错误码
 		<< ", task id: " << cbEvent->getTaskId()    // 当前任务的task id，方便定位问题，建议输出
 		<< ", result: " << cbEvent->getResult()  // 获取中间识别结果
 		<< endl;
//	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " OnResultDataRecved %s %s\n", str->getId().c_str(), str->getResponse().c_str());
    const char*pmainuuid=(const char*)cbParam;

    switch_event_t *event = NULL;
    if (switch_event_create(&event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS) {

        event->subclass_name = strdup("asr");
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Subclass", event->subclass_name);
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "ASR-Response", cbEvent->getAllResponse());
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel", cbEvent->getTaskId());
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "mainUUID",pmainuuid);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " OnResultDataRecved pvt->mainUUID:%s\n", pmainuuid);

        switch_event_fire(&event);
    }
	// cout << "OnRecognitionCompleted: All response:" << cbEvent->getAllResponse() << endl; // 获取服务端返回的全部信息
}

/**
* @brief 识别过程(包含start(), send(), stop())发生异常时, sdk内部线程上报TaskFailed事件
* @note 上报TaskFailed事件之后, SDK内部会关闭识别连接通道. 此时调用sendAudio会返回-1, 请停止发送.
*       不允许在回调函数内部调用stop(), releaseRecognizerRequest()对象操作, 否则会异常
* @param cbEvent 回调事件结构, 详见nlsEvent.h
* @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
* @return
*/
void OnRecognitionTaskFailed(NlsEvent* cbEvent, void* cbParam) {

	cout << "OnRecognitionTaskFailed: "
		<< "status code: " << cbEvent->getStausCode() // 获取消息的状态码，成功为0或者20000000，失败时对应失败的错误码
		<< ", task id: " << cbEvent->getTaskId()    // 当前任务的task id，方便定位问题，建议输出
		<< ", error message: " << cbEvent->getErrorMessage()
		<< endl;
	// cout << "OnRecognitionTaskFailed: All response:" << cbEvent->getAllResponse() << endl; // 获取服务端返回的全部信息
}

/**
* @brief 识别结束或发生异常时，会关闭连接通道, sdk内部线程上报ChannelCloseed事件
* @note 不允许在回调函数内部调用stop(), releaseRecognizerRequest()对象操作, 否则会异常
* @param cbEvent 回调事件结构, 详见nlsEvent.h
* @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
* @return
*/
void OnRecognitionChannelCloseed(NlsEvent* cbEvent, void* cbParam) {

	cout << "OnRecognitionChannelCloseed: All response:" << cbEvent->getAllResponse() << endl; // 获取服务端返回的全部信息
}

static switch_bool_t asr_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	//pthread_mutex_lock(&mutex);
    switch_da_t *pvt = (switch_da_t *)user_data;
    switch_channel_t *channel = switch_core_session_get_channel(pvt->session);

    switch (type) {
    case SWITCH_ABC_TYPE_INIT:
        {

			pvt->callback = new SpeechRecognizerCallback();
			pvt->callback->setOnRecognitionStarted(OnRecognitionStarted, (void*)((const char*)pvt->mainUUID)); // 设置start()成功回调函数
			pvt->callback->setOnTaskFailed(OnRecognitionTaskFailed, (void*)((const char*)pvt->mainUUID)); // 设置异常识别回调函数
			pvt->callback->setOnChannelClosed(OnRecognitionChannelCloseed, (void*)((const char*)pvt->mainUUID)); // 设置识别通道关闭回调函数
			pvt->callback->setOnRecognitionResultChanged(OnRecognitionResultChanged, (void*)((const char*)pvt->mainUUID)); // 设置中间结果回调函数
			pvt->callback->setOnRecognitionCompleted(OnRecognitionCompleted, (void*)((const char*)pvt->mainUUID)); // 设置识别结束回调函数
			pvt->request = NlsClient::getInstance()->createRecognizerRequest(pvt->callback);
			if (pvt->request == NULL) {
				cout << "createRecognizerRequest failed." << endl;

				delete pvt->callback;
				pvt->callback = NULL;

				return SWITCH_FALSE;
			}
			pvt->request->setAppKey(pvt->appkey); // 设置AppKey, 必填参数, 请参照官网申请
			pvt->request->setFormat("pcm"); // 设置音频数据编码格式, 可选参数, 目前支持pcm, opus. 默认是pcm
			pvt->request->setSampleRate(SAMPLE_RATE_8000); // 设置音频数据采样率, 可选参数, 目前支持16000, 8000. 默认是16000
			pvt->request->setIntermediateResult(true); // 设置是否返回中间识别结果, 可选参数. 默认false
			pvt->request->setPunctuationPrediction(true); // 设置是否在后处理中添加标点, 可选参数. 默认false
			pvt->request->setInverseTextNormalization(true); // 设置是否在后处理中执行ITN, 可选参数. 默认false
			string token;
			long expireTime=-1;
			generateToken(pvt->id,pvt->seceret,&token,&expireTime);
			pvt->request->setToken(token.c_str()); // 设置账号校验token, 必填参数
			if (pvt->request->start() < 0) {
				cout << "start() failed." << endl;
				NlsClient::getInstance()->releaseRecognizerRequest(pvt->request); // start()失败，释放request对象
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "ASR Start Failed channel:%s\n", switch_channel_get_name(channel));
				delete pvt->callback;
				pvt->callback = NULL;
				return SWITCH_FALSE;
			}
			else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "ASR Start Succeed channel:%s\n", switch_channel_get_name(channel));
			}
		}
        break;
    case SWITCH_ABC_TYPE_CLOSE:
        {
        if (pvt->request) {

				pvt->request->stop();
	            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "ASR Stop Succeed channel:%s\n", switch_channel_get_name(channel));
				NlsClient::getInstance()->releaseRecognizerRequest(pvt->request);
				delete pvt->callback;
				pvt->callback = NULL;
            }
        }
        break;

    case SWITCH_ABC_TYPE_READ_REPLACE:
        {
  
			
            switch_frame_t *frame;
            if ((frame = switch_core_media_bug_get_read_replace_frame(bug)))
			{
                char*frame_data = (char*)frame->data;
                int frame_len = frame->datalen;
                switch_core_media_bug_set_read_replace_frame(bug, frame);
                
                if (frame->channels != 1)
                {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "nonsupport channels:%d!\n",frame->channels);
                    return SWITCH_FALSE;
                }

                if (pvt->request) {
// 					if (pvt->request->start() < 0) {
// 						cout << "start() failed." << endl;
// 						NlsClient::getInstance()->releaseRecognizerRequest(pvt->request); // start()失败，释放request对象
// 						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "ASR Start Failed channel:%s\n", switch_channel_get_name(channel));
// 						delete pvt->callback;
// 						pvt->callback = NULL;
// 						return SWITCH_FALSE;
// 					}
// 					else {
// 						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "ASR Start Succeed channel:%s\n", switch_channel_get_name(channel));
// 					}
		            if (pvt->request->sendAudio(frame_data, frame_len,false) <= 0) {
                        return SWITCH_FALSE;
                    }
					// pvt->request->stop();
                }
            }
 
        }
        break;
    default: break;
    }
	//pthread_mutex_unlock(&mutex);
    return SWITCH_TRUE;
}


SWITCH_STANDARD_APP(stop_asr_session_function)
{
    switch_da_t *pvt;
    switch_channel_t *channel = switch_core_session_get_channel(session);
	 switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "%s Come into stop\n", switch_channel_get_name(channel));
    if ((pvt = (switch_da_t*)switch_channel_get_private(channel, "asr"))) {

        switch_channel_set_private(channel, "asr", NULL);
        switch_core_media_bug_remove(session, &pvt->bug);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s Stop ASR\n", switch_channel_get_name(channel));

    }
}


SWITCH_STANDARD_APP(start_asr_session_function)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Come in start\n");
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_status_t status;
    switch_da_t *pvt;
    switch_codec_implementation_t read_impl;
    memset(&read_impl, 0, sizeof(switch_codec_implementation_t));

    char *argv[4] = { 0 };
    int argc;
    char *lbuf = NULL;
	//NlsClient *nlc=NULL;
	//nlc = NlsClient::getInstance();
    if (!zstr(data) && (lbuf = switch_core_session_strdup(session, data))
        && (argc = switch_separate_string(lbuf, ' ', argv, (sizeof(argv) / sizeof(argv[0])))) >= 4) {

        switch_core_session_get_read_impl(session, &read_impl);

        if (!(pvt = (switch_da_t*)switch_core_session_alloc(session, sizeof(switch_da_t)))) {
            return;
        }

        pvt->stop = 0;
        pvt->session = session;
        pvt->id = argv[0];
        pvt->seceret = argv[1];
		pvt->mainUUID=argv[2];
		pvt->appkey=argv[3];
	


        if ((status = switch_core_media_bug_add(session, "asr", NULL,
            asr_callback, pvt, 0, SMBF_READ_REPLACE | SMBF_NO_PAUSE | SMBF_ONE_ONLY, &(pvt->bug))) != SWITCH_STATUS_SUCCESS) {
            return;
        }

        switch_channel_set_private(channel, "asr", pvt);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s Start ASR\n", switch_channel_get_name(channel));
    }
    else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "%s id or secret can not be empty\n", switch_channel_get_name(channel));
    }
	
    
}






SWITCH_MODULE_LOAD_FUNCTION(mod_asr_load)
{
    switch_application_interface_t *app_interface;

    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    SWITCH_ADD_APP(app_interface, "start_asr", "asr", "asr",start_asr_session_function, "", SAF_MEDIA_TAP);
    SWITCH_ADD_APP(app_interface, "stop_asr", "asr", "asr", stop_asr_session_function, "", SAF_NONE);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, " asr_load\n");
	int ret = NlsClient::getInstance()->setLogConfig("log-recognizer.txt", LogInfo);
	if (-1 == ret) {
		cout << "set log failed." << endl;
		return SWITCH_STATUS_SUCCESS;
	}
    return SWITCH_STATUS_SUCCESS;
}


 SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_asr_shutdown)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, " asr_shutdown\n");
	NlsClient::releaseInstance();
    return SWITCH_STATUS_SUCCESS;
}
