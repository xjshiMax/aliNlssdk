/* asr mod. Copyright 2017, xuhuaiyi ALL RIGHTS RESERVED!
    Author: cdevelop@qq.com(wwww.ddrj.com)

    ֮ǰд��һ����ҵģ��mod_vad���Լ����ο����ӿ�SmartIVR(RESTful API)��ϸ�� http://www.dingdingtong.cn/smartivr/
    �ܶ��û�ֻ�����һ��ASR��Ч��,�Լ�ϣ���Լ����ջ�ȡFreeSWITCH�������ݵ�ԭ��͹��̣���������д�������Դ��Ŀ����Ҳο���
    ����Ŀ�ļ����������Լ�Ⱥ��340129771
    �����Ҫ��ҵ�����֧�ֿ�����ϵQQ��1280791187������΢�ţ�cdevelop
    
    ����Ŀ��ͨ��������ʵʱ����ʶ��ӿڰ�ͨ���е����������͵�����ʶ���������ʶ����ͨ��ESL�¼�֪ͨҵ�����


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
* ȫ��ά��һ�������Ȩtoken�����Ӧ����Ч��ʱ�����
* ÿ�ε��÷���֮ǰ�������ж�token�Ƿ��Ѿ����ڣ�
* ����Ѿ����ڣ������AccessKey ID��AccessKey Secret��������һ��token�����������ȫ�ֵ�token������Ч��ʱ�����
*
* ע�⣺��Ҫÿ�ε��÷���֮ǰ������������token��ֻ����token��������ʱ�������ɼ��ɡ����еķ��񲢷��ɹ���һ��token��
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
* ����AccessKey ID��AccessKey Secret��������һ��token������ȡ����Ч��ʱ���
*/
int generateToken(string akId, string akSecret, string* token, long* expireTime) {
	NlsToken nlsTokenRequest;
	nlsTokenRequest.setAccessKeyId(akId);
	nlsTokenRequest.setKeySecret(akSecret);

	if (-1 == nlsTokenRequest.applyNlsToken()) {
		cout << "Failed: " << nlsTokenRequest.getErrorMsg() << endl; /*��ȡʧ��ԭ��*/

		return -1;
	}

	*token = nlsTokenRequest.getToken();
	*expireTime = nlsTokenRequest.getExpireTime();

	return 0;
}


/**
* @brief ����start(), �ɹ����ƶ˽�������, sdk�ڲ��߳��ϱ�started�¼�
* @note �������ڻص������ڲ�����stop(), releaseRecognizerRequest()�������, ������쳣
* @param cbEvent �ص��¼��ṹ, ���nlsEvent.h
* @param cbParam �ص��Զ��������Ĭ��ΪNULL, ���Ը��������Զ������
* @return
*/
void OnRecognitionStarted(NlsEvent* cbEvent, void* cbParam) {
	cout << "OnRecognitionStarted: "
		<< "status code: " << cbEvent->getStausCode()  // ��ȡ��Ϣ��״̬�룬�ɹ�Ϊ0����20000000��ʧ��ʱ��Ӧʧ�ܵĴ�����
		<< ", task id: " << cbEvent->getTaskId()   // ��ǰ�����task id�����㶨λ���⣬�������
		<< endl;
	// cout << "OnRecognitionStarted: All response:" << cbEvent->getAllResponse() << endl; // ��ȡ����˷��ص�ȫ����Ϣ
}

/**
* @brief �����������м�������, sdk�ڽ��յ��ƶ˷��ص��м���ʱ, sdk�ڲ��߳��ϱ�ResultChanged�¼�
* @note �������ڻص������ڲ�����stop(), releaseRecognizerRequest()�������, ������쳣
* @param cbEvent �ص��¼��ṹ, ���nlsEvent.h
* @param cbParam �ص��Զ��������Ĭ��ΪNULL, ���Ը��������Զ������
* @return
*/
void OnRecognitionResultChanged(NlsEvent* cbEvent, void* cbParam) {
	
	cout << "OnRecognitionResultChanged: "
		<< "status code: " << cbEvent->getStausCode()  // ��ȡ��Ϣ��״̬�룬�ɹ�Ϊ0����20000000��ʧ��ʱ��Ӧʧ�ܵĴ�����
		<< ", task id: " << cbEvent->getTaskId()    // ��ǰ�����task id�����㶨λ���⣬�������
		<< ", result: " << cbEvent->getResult()     // ��ȡ�м�ʶ����
		<< endl;
	// cout << "OnRecognitionResultChanged: All response:" << cbEvent->getAllResponse() << endl; // ��ȡ����˷��ص�ȫ����Ϣ
}

/**
* @brief sdk�ڽ��յ��ƶ˷���ʶ�������Ϣʱ, sdk�ڲ��߳��ϱ�Completed�¼�
* @note �ϱ�Completed�¼�֮��, SDK�ڲ���ر�ʶ������ͨ��. ��ʱ����sendAudio�᷵��-1, ��ֹͣ����.
*       �������ڻص������ڲ�����stop(), releaseRecognizerRequest()�������, ������쳣.
* @param cbEvent �ص��¼��ṹ, ���nlsEvent.h
* @param cbParam �ص��Զ��������Ĭ��ΪNULL, ���Ը��������Զ������
* @return
*/
void OnRecognitionCompleted(NlsEvent* cbEvent, void* cbParam) {
// 	ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
// 	cout << "CbParam: " << tmpParam->iExg << " " << tmpParam->sExg << endl; // ����ʾ�Զ������ʾ��
// 
 	cout << "OnRecognitionCompleted: "
 		<< "status code: " << cbEvent->getStausCode()  // ��ȡ��Ϣ��״̬�룬�ɹ�Ϊ0����20000000��ʧ��ʱ��Ӧʧ�ܵĴ�����
 		<< ", task id: " << cbEvent->getTaskId()    // ��ǰ�����task id�����㶨λ���⣬�������
 		<< ", result: " << cbEvent->getResult()  // ��ȡ�м�ʶ����
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
	// cout << "OnRecognitionCompleted: All response:" << cbEvent->getAllResponse() << endl; // ��ȡ����˷��ص�ȫ����Ϣ
}

/**
* @brief ʶ�����(����start(), send(), stop())�����쳣ʱ, sdk�ڲ��߳��ϱ�TaskFailed�¼�
* @note �ϱ�TaskFailed�¼�֮��, SDK�ڲ���ر�ʶ������ͨ��. ��ʱ����sendAudio�᷵��-1, ��ֹͣ����.
*       �������ڻص������ڲ�����stop(), releaseRecognizerRequest()�������, ������쳣
* @param cbEvent �ص��¼��ṹ, ���nlsEvent.h
* @param cbParam �ص��Զ��������Ĭ��ΪNULL, ���Ը��������Զ������
* @return
*/
void OnRecognitionTaskFailed(NlsEvent* cbEvent, void* cbParam) {

	cout << "OnRecognitionTaskFailed: "
		<< "status code: " << cbEvent->getStausCode() // ��ȡ��Ϣ��״̬�룬�ɹ�Ϊ0����20000000��ʧ��ʱ��Ӧʧ�ܵĴ�����
		<< ", task id: " << cbEvent->getTaskId()    // ��ǰ�����task id�����㶨λ���⣬�������
		<< ", error message: " << cbEvent->getErrorMessage()
		<< endl;
	// cout << "OnRecognitionTaskFailed: All response:" << cbEvent->getAllResponse() << endl; // ��ȡ����˷��ص�ȫ����Ϣ
}

/**
* @brief ʶ����������쳣ʱ����ر�����ͨ��, sdk�ڲ��߳��ϱ�ChannelCloseed�¼�
* @note �������ڻص������ڲ�����stop(), releaseRecognizerRequest()�������, ������쳣
* @param cbEvent �ص��¼��ṹ, ���nlsEvent.h
* @param cbParam �ص��Զ��������Ĭ��ΪNULL, ���Ը��������Զ������
* @return
*/
void OnRecognitionChannelCloseed(NlsEvent* cbEvent, void* cbParam) {

	cout << "OnRecognitionChannelCloseed: All response:" << cbEvent->getAllResponse() << endl; // ��ȡ����˷��ص�ȫ����Ϣ
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
			pvt->callback->setOnRecognitionStarted(OnRecognitionStarted, (void*)((const char*)pvt->mainUUID)); // ����start()�ɹ��ص�����
			pvt->callback->setOnTaskFailed(OnRecognitionTaskFailed, (void*)((const char*)pvt->mainUUID)); // �����쳣ʶ��ص�����
			pvt->callback->setOnChannelClosed(OnRecognitionChannelCloseed, (void*)((const char*)pvt->mainUUID)); // ����ʶ��ͨ���رջص�����
			pvt->callback->setOnRecognitionResultChanged(OnRecognitionResultChanged, (void*)((const char*)pvt->mainUUID)); // �����м����ص�����
			pvt->callback->setOnRecognitionCompleted(OnRecognitionCompleted, (void*)((const char*)pvt->mainUUID)); // ����ʶ������ص�����
			pvt->request = NlsClient::getInstance()->createRecognizerRequest(pvt->callback);
			if (pvt->request == NULL) {
				cout << "createRecognizerRequest failed." << endl;

				delete pvt->callback;
				pvt->callback = NULL;

				return SWITCH_FALSE;
			}
			pvt->request->setAppKey(pvt->appkey); // ����AppKey, �������, ����չ�������
			pvt->request->setFormat("pcm"); // ������Ƶ���ݱ����ʽ, ��ѡ����, Ŀǰ֧��pcm, opus. Ĭ����pcm
			pvt->request->setSampleRate(SAMPLE_RATE_8000); // ������Ƶ���ݲ�����, ��ѡ����, Ŀǰ֧��16000, 8000. Ĭ����16000
			pvt->request->setIntermediateResult(true); // �����Ƿ񷵻��м�ʶ����, ��ѡ����. Ĭ��false
			pvt->request->setPunctuationPrediction(true); // �����Ƿ��ں�������ӱ��, ��ѡ����. Ĭ��false
			pvt->request->setInverseTextNormalization(true); // �����Ƿ��ں�����ִ��ITN, ��ѡ����. Ĭ��false
			string token;
			long expireTime=-1;
			generateToken(pvt->id,pvt->seceret,&token,&expireTime);
			pvt->request->setToken(token.c_str()); // �����˺�У��token, �������
			if (pvt->request->start() < 0) {
				cout << "start() failed." << endl;
				NlsClient::getInstance()->releaseRecognizerRequest(pvt->request); // start()ʧ�ܣ��ͷ�request����
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
// 						NlsClient::getInstance()->releaseRecognizerRequest(pvt->request); // start()ʧ�ܣ��ͷ�request����
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
