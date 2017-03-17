// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifdef __cplusplus
#include <cstdlib>
#include <cstddef>
#include <cstring>
#else
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include "umock_c.h"
#endif

#include "testrunnerswitcher.h"

#include "iothub_client.h"
#include "iothub_client_options.h"
#include "iothub_message.h"
#include "iothub_messaging.h"

#include "iothub_account.h"
#include "iothubtest.h"

#include "azure_c_shared_utility/buffer_.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/platform.h"
#include "../../../certs/certs.h"

#include "iothubclient_common_e2e.h"

static bool g_callbackRecv = false;

const char* TEST_EVENT_DATA_FMT = "{\"data\":\"%.24s\",\"id\":\"%d\"}";

const char* MSG_ID = "MessageIdForE2E";
const char* MSG_CORRELATION_ID = "MessageCorrelationIdForE2E";
const char* MSG_CONTENT = "Message content for E2E";

#define MSG_PROP_COUNT 3
const char* MSG_PROP_KEYS[MSG_PROP_COUNT] = { "Key1", "Key2", "Key3" };
const char* MSG_PROP_VALS[MSG_PROP_COUNT] = { "Val1", "Val2", "Val3" };

static size_t g_iotHubTestId = 0;
IOTHUB_ACCOUNT_INFO_HANDLE g_iothubAcctInfo = NULL;

#define IOTHUB_COUNTER_MAX           10
#define IOTHUB_TIMEOUT_SEC           1000
#define MAX_CLOUD_TRAVEL_TIME        20.0

TEST_DEFINE_ENUM_TYPE(IOTHUB_TEST_CLIENT_RESULT, IOTHUB_TEST_CLIENT_RESULT_VALUES);
TEST_DEFINE_ENUM_TYPE(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_RESULT_VALUES);

typedef struct EXPECTED_SEND_DATA_TAG
{
    const char* expectedString;
    bool wasFound;
    bool dataWasRecv;
    LOCK_HANDLE lock;
    IOTHUB_MESSAGE_HANDLE msgHandle;
} EXPECTED_SEND_DATA;

typedef struct EXPECTED_RECEIVE_DATA_TAG
{
    bool wasFound;
    LOCK_HANDLE lock; /*needed to protect this structure*/
    IOTHUB_MESSAGE_HANDLE msgHandle;
} EXPECTED_RECEIVE_DATA;

static void openCompleteCallback(void* context)
{
    (void)printf("Open completed, context: %s\n", (char*)context);
}

static void sendCompleteCallback(void* context, IOTHUB_MESSAGING_RESULT messagingResult)
{
    (void)context;
    if (messagingResult == IOTHUB_MESSAGING_OK)
    {
        (void)printf("Message has been sent successfully\n");
    }
    else
    {
        (void)printf("Send failed\n");
    }
}

static int IoTHubCallback(void* context, const char* data, size_t size)
{
    (void)size;
    int result = 0; // 0 means "keep processing"
    EXPECTED_SEND_DATA* expectedData = (EXPECTED_SEND_DATA*)context;
    if (expectedData != NULL)
    {
        if (Lock(expectedData->lock) != LOCK_OK)
        {
            ASSERT_FAIL("unable to lock");
        }
        else
        {
            if (
                (strlen(expectedData->expectedString) == size) &&
                (memcmp(expectedData->expectedString, data, size) == 0)
                )
            {
                expectedData->wasFound = true;
                result = 1;
            }
            (void)Unlock(expectedData->lock);
        }
    }
    return result;
}

static void ReceiveConfirmationCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
{
    (void)result;
    EXPECTED_SEND_DATA* expectedData = (EXPECTED_SEND_DATA*)userContextCallback;
    if (expectedData != NULL)
    {
        if (Lock(expectedData->lock) != LOCK_OK)
        {
            ASSERT_FAIL("unable to lock");
        }
        else
        {
            expectedData->dataWasRecv = true;
            (void)Unlock(expectedData->lock);
        }
    }
}

static IOTHUBMESSAGE_DISPOSITION_RESULT ReceiveMessageCallback(IOTHUB_MESSAGE_HANDLE messageHandle, void* userContextCallback)
{
    EXPECTED_RECEIVE_DATA* receiveUserContext = (EXPECTED_RECEIVE_DATA*)userContextCallback;
    if (receiveUserContext == NULL)
    {
        ASSERT_FAIL("User context is NULL");
    }
    else
    {
        if (Lock(receiveUserContext->lock) != LOCK_OK)
        {
            ASSERT_FAIL("Unable to lock"); /*because the test must absolutely be terminated*/
        }
        else
        {
            const char* messageId;
            const char* correlationId;
            const unsigned char* content;
            size_t contentSize;

            if ((messageId = IoTHubMessage_GetMessageId(messageHandle)) == NULL)
            {
                messageId = "<null>";
            }
            else
            {
                if (strcmp(messageId, MSG_ID) != 0)
                {
                    ASSERT_FAIL("Message ID mismatch.");
                }
            }

            if ((correlationId = IoTHubMessage_GetCorrelationId(messageHandle)) == NULL)
            {
                correlationId = "<null>";
            }
            else
            {
                if (strcmp(correlationId, MSG_CORRELATION_ID) != 0)
                {
                    ASSERT_FAIL("Message correlation ID mismatch.");
                }
            }

            IOTHUBMESSAGE_CONTENT_TYPE contentType = IoTHubMessage_GetContentType(messageHandle);
            if (contentType != IOTHUBMESSAGE_BYTEARRAY)
            {
                ASSERT_FAIL("Message content type mismatch.");
            }
            else
            {
                if (IoTHubMessage_GetByteArray(messageHandle, &content, &contentSize) != IOTHUB_MESSAGE_OK)
                {
                    ASSERT_FAIL("Unable to get the content of the message.");
                }
                else
                {
                    (void)printf("Received new message from IoT Hub :\r\nMessage-id: %s\r\nCorrelation-id: %s\r\n", messageId, correlationId);
                    (void)printf("\r\n");
                }

                receiveUserContext->wasFound = true;
                MAP_HANDLE mapHandle = IoTHubMessage_Properties(messageHandle);
                if (mapHandle != NULL)
                {
                    const char*const* keys;
                    const char*const* values;
                    size_t propertyCount = 0;
                    if (Map_GetInternals(mapHandle, &keys, &values, &propertyCount) == MAP_OK)
                    {
                        receiveUserContext->wasFound = true;
                        if (propertyCount == MSG_PROP_COUNT)
                        {
                            (void)printf("Message Properties:\r\n");
                            for (size_t index = 0; index < propertyCount; index++)
                            {
                                (void)printf("\tKey: %s Value: %s\r\n", keys[index], values[index]);
                                if (strcmp(keys[index], MSG_PROP_KEYS[index]) != 0)
                                {
                                    receiveUserContext->wasFound = false;
                                }
                                if (strcmp(values[index], MSG_PROP_VALS[index]) != 0)
                                {
                                    receiveUserContext->wasFound = false;
                                }
                            }
                            (void)printf("\r\n");
                        }
                        else
                        {
                            receiveUserContext->wasFound = false;
                        }
                    }
                }
            }
            Unlock(receiveUserContext->lock);
        }
    }
    return IOTHUBMESSAGE_ACCEPTED;
}

static EXPECTED_RECEIVE_DATA* ReceiveUserContext_Create(void)
{
    EXPECTED_RECEIVE_DATA* result = (EXPECTED_RECEIVE_DATA*)malloc(sizeof(EXPECTED_RECEIVE_DATA));
    if (result != NULL)
    {
        memset(result, 0, sizeof(*result));
        if ((result->lock = Lock_Init()) == NULL)
        {
            free(result);
            result = NULL;
        }
    }
    return result;
}

static void ReceiveUserContext_Destroy(EXPECTED_RECEIVE_DATA* data)
{
    if (data != NULL)
    {
        (void)Lock_Deinit(data->lock);

        if (data->msgHandle != NULL)
        {
            IoTHubMessage_Destroy(data->msgHandle);
        }

        free(data);
    }
}

static EXPECTED_SEND_DATA* EventData_Create(void)
{
    EXPECTED_SEND_DATA* result = (EXPECTED_SEND_DATA*)malloc(sizeof(EXPECTED_SEND_DATA));
    memset(result, 0, sizeof(*result));
    if (result != NULL)
    {
        if ((result->lock = Lock_Init()) == NULL)
        {
            ASSERT_FAIL("unable to Lock_Init");
        }
        else
        {
            char temp[1000];
            char* tempString;
            time_t t = time(NULL);
            size_t string_length;
            string_length = sprintf(temp, TEST_EVENT_DATA_FMT, ctime(&t), g_iotHubTestId);
            if ((tempString = (char*)malloc(string_length + 1)) == NULL)
            {
                Lock_Deinit(result->lock);
                free(result);
                result = NULL;
            }
            else
            {
                strcpy(tempString, temp);
                result->expectedString = tempString;
                result->wasFound = false;
                result->dataWasRecv = false;
            }
        }
    }
    return result;
}

static void EventData_Destroy(EXPECTED_SEND_DATA* data)
{
    if (data != NULL)
    {
        (void)Lock_Deinit(data->lock);
        if (data->expectedString != NULL)
        {
            free((void*)data->expectedString);
        }
        if (data->msgHandle != NULL)
        {
            IoTHubMessage_Destroy(data->msgHandle);
        }
        free(data);
    }
}


void e2e_init(void)
{
    int result = platform_init();
    ASSERT_ARE_EQUAL_WITH_MSG(int, 0, result, "Platform init failed");
    g_iothubAcctInfo = IoTHubAccount_Init();
    ASSERT_IS_NOT_NULL_WITH_MSG(g_iothubAcctInfo, "Could not initialize IoTHubAccount");
    platform_init();
}

void e2e_deinit(void)
{
    IoTHubAccount_deinit(g_iothubAcctInfo);
    // Need a double deinit
    platform_deinit();
    platform_deinit();
}

IOTHUB_CLIENT_HANDLE client_connect_to_hub(IOTHUB_PROVISIONED_DEVICE* deviceToUse, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    IOTHUB_CLIENT_HANDLE iotHubClientHandle;
    IOTHUB_CLIENT_RESULT result;

    iotHubClientHandle = IoTHubClient_CreateFromConnectionString(deviceToUse->connectionString, protocol);
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubClientHandle, "Could not create IoTHubClient");

    if (deviceToUse->howToCreate == IOTHUB_ACCOUNT_AUTH_X509) {
        result = IoTHubClient_SetOption(iotHubClientHandle, OPTION_X509_CERT, deviceToUse->certificate);
        ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set the device x509 certificate");
        result = IoTHubClient_SetOption(iotHubClientHandle, OPTION_X509_PRIVATE_KEY, deviceToUse->primaryAuthentication);
        ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set the device x509 privateKey");
    }

    // Turn on Log 
    bool trace = true;
    (void)IoTHubClient_SetOption(iotHubClientHandle, OPTION_LOG_TRACE, &trace);
    (void)IoTHubClient_SetOption(iotHubClientHandle, "TrustedCerts", certificates);

    return iotHubClientHandle;
}

D2C_MESSAGE_HANDLE client_create_and_send_d2c(IOTHUB_CLIENT_HANDLE iotHubClientHandle)
{
    IOTHUB_MESSAGE_HANDLE msgHandle;
    IOTHUB_CLIENT_RESULT result;

    EXPECTED_SEND_DATA* sendData = EventData_Create();
    ASSERT_IS_NOT_NULL_WITH_MSG(sendData, "Could not create the EventData associated with the event to be sent");

    msgHandle = IoTHubMessage_CreateFromByteArray((const unsigned char*)sendData->expectedString, strlen(sendData->expectedString));
    ASSERT_IS_NOT_NULL_WITH_MSG(msgHandle, "Could not create the D2C message to be sent");

    MAP_HANDLE mapHandle = IoTHubMessage_Properties(msgHandle);
    for (size_t i = 0; i < MSG_PROP_COUNT; i++)
    {
        if (Map_AddOrUpdate(mapHandle, MSG_PROP_KEYS[i], MSG_PROP_VALS[i]) != MAP_OK)
        {
            (void)printf("ERROR: Map_AddOrUpdate failed for property %zu!\r\n", i);
        }
    }

    sendData->msgHandle = msgHandle;

    // act
    result = IoTHubClient_SendEventAsync(iotHubClientHandle, msgHandle, ReceiveConfirmationCallback, sendData);
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "SendEventAsync failed");

    return (D2C_MESSAGE_HANDLE)sendData;
}

bool client_wait_for_d2c_confirmation(D2C_MESSAGE_HANDLE d2cMessage)
{
    time_t beginOperation, nowTime;
    bool result = false;
    EXPECTED_SEND_DATA* sendData = (EXPECTED_SEND_DATA*)d2cMessage;

    beginOperation = time(NULL);
    while (
        (nowTime = time(NULL)),
        (difftime(nowTime, beginOperation) < MAX_CLOUD_TRAVEL_TIME) // time box
        )
    {
        if (Lock(sendData->lock) != LOCK_OK)
        {
            ASSERT_FAIL("unable to lock");
        }
        else
        {
            if (sendData->dataWasRecv)
            {
                Unlock(sendData->lock);
                break;
            }
            Unlock(sendData->lock);
        }
        ThreadAPI_Sleep(100);
    }

    if (Lock(sendData->lock) != LOCK_OK)
    {
        ASSERT_FAIL("unable to lock");
    }
    else
    {
        result = sendData->dataWasRecv;
        (void)Unlock(sendData->lock);
    }

    return result;

}

bool client_received_confirmation(D2C_MESSAGE_HANDLE d2cMessage)
{
    return ((EXPECTED_SEND_DATA*)d2cMessage)->dataWasRecv;
}

void service_wait_for_d2c_event_arrival(IOTHUB_PROVISIONED_DEVICE* deviceToUse, D2C_MESSAGE_HANDLE d2cMessage)
{
    EXPECTED_SEND_DATA* sendData = (EXPECTED_SEND_DATA*)d2cMessage;
    
    IOTHUB_TEST_HANDLE iotHubTestHandle = IoTHubTest_Initialize(IoTHubAccount_GetEventHubConnectionString(g_iothubAcctInfo), IoTHubAccount_GetIoTHubConnString(g_iothubAcctInfo), deviceToUse->deviceId, IoTHubAccount_GetEventhubListenName(g_iothubAcctInfo), IoTHubAccount_GetEventhubAccessKey(g_iothubAcctInfo), IoTHubAccount_GetSharedAccessSignature(g_iothubAcctInfo), IoTHubAccount_GetEventhubConsumerGroup(g_iothubAcctInfo));
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubTestHandle, "Could not initialize IoTHubTest in order to listen for events");

    IOTHUB_TEST_CLIENT_RESULT result = IoTHubTest_ListenForEventForMaxDrainTime(iotHubTestHandle, IoTHubCallback, IoTHubAccount_GetIoTHubPartitionCount(g_iothubAcctInfo), sendData);
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_TEST_CLIENT_RESULT, IOTHUB_TEST_CLIENT_OK, result, "Listening for the event failed");
    
    ASSERT_IS_TRUE_WITH_MSG(sendData->wasFound, "Failure retrieving data that was sent to eventhub"); // was found is written by the callback...

    IoTHubTest_Deinit(iotHubTestHandle);
}

bool service_received_the_message(D2C_MESSAGE_HANDLE d2cMessage)
{
    return ((EXPECTED_SEND_DATA*)d2cMessage)->wasFound;
}

void destroy_d2c_message_handle(D2C_MESSAGE_HANDLE d2cMessage)
{
    EventData_Destroy((EXPECTED_SEND_DATA*)d2cMessage);
}

static void send_event_test(IOTHUB_PROVISIONED_DEVICE* deviceToUse, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{

    // arrange
    IOTHUB_CLIENT_HANDLE iotHubClientHandle;
    D2C_MESSAGE_HANDLE d2cMessage;

    // Create the IoT Hub Data
    iotHubClientHandle = client_connect_to_hub(deviceToUse, protocol);

    // Send the Event from the client
    d2cMessage = client_create_and_send_d2c(iotHubClientHandle);

    // Wait for confirmation that the event was recevied
    bool dataWasRecv = client_wait_for_d2c_confirmation(d2cMessage);
    ASSERT_IS_TRUE_WITH_MSG(dataWasRecv, "Failure sending data to IotHub"); // was received by the callback...

    // close the client connection
    IoTHubClient_Destroy(iotHubClientHandle);

    /* guess who */
    (void)platform_init();

    // Waigt for the message to arrive
    service_wait_for_d2c_event_arrival(deviceToUse, d2cMessage);

    // cleanup
    destroy_d2c_message_handle(d2cMessage);


}

void e2e_send_event_test_sas(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    send_event_test(IoTHubAccount_GetSASDevice(g_iothubAcctInfo), protocol);
}

void e2e_send_event_test_x509(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    send_event_test(IoTHubAccount_GetX509Device(g_iothubAcctInfo), protocol);
}


EXPECTED_RECEIVE_DATA *service_create_c2d(const char *content)
{
    IOTHUB_MESSAGE_RESULT iotHubMessageResult;
    IOTHUB_MESSAGE_HANDLE messageHandle;

    EXPECTED_RECEIVE_DATA *receiveUserContext = ReceiveUserContext_Create();
    ASSERT_IS_NOT_NULL_WITH_MSG(receiveUserContext, "Could not create receive user context");

    messageHandle = IoTHubMessage_CreateFromString(content);
    ASSERT_IS_NOT_NULL_WITH_MSG(messageHandle, "Could not create IoTHubMessage to send C2D messages to the device");

    iotHubMessageResult = IoTHubMessage_SetMessageId(messageHandle, MSG_ID);
    ASSERT_ARE_EQUAL(int, IOTHUB_MESSAGING_OK, iotHubMessageResult);

    iotHubMessageResult = IoTHubMessage_SetCorrelationId(messageHandle, MSG_CORRELATION_ID);
    ASSERT_ARE_EQUAL(int, IOTHUB_MESSAGING_OK, iotHubMessageResult);

    MAP_HANDLE mapHandle = IoTHubMessage_Properties(messageHandle);
    for (size_t i = 0; i < MSG_PROP_COUNT; i++)
    {
        if (Map_AddOrUpdate(mapHandle, MSG_PROP_KEYS[i], MSG_PROP_VALS[i]) != MAP_OK)
        {
            (void)printf("ERROR: Map_AddOrUpdate failed for property %zu!\r\n", i);
        }
    }

    receiveUserContext->msgHandle = messageHandle;

    return receiveUserContext;
}

static void service_send_c2d(IOTHUB_MESSAGING_CLIENT_HANDLE iotHubMessagingHandle, EXPECTED_RECEIVE_DATA* receiveUserContext, IOTHUB_PROVISIONED_DEVICE* deviceToUse)
{
    IOTHUB_MESSAGING_RESULT iotHubMessagingResult;

    iotHubMessagingResult = IoTHubMessaging_SendAsync(iotHubMessagingHandle, deviceToUse->deviceId, receiveUserContext->msgHandle, sendCompleteCallback, receiveUserContext);
    ASSERT_ARE_EQUAL_WITH_MSG(int, IOTHUB_MESSAGING_OK, iotHubMessagingResult, "IoTHubMessaging_SendAsync failed, could not send C2D message to the device");
}


void client_wait_for_c2d_event_arrival(EXPECTED_RECEIVE_DATA* receiveUserContext)
{
    time_t beginOperation, nowTime;

    beginOperation = time(NULL);
    while (
        (nowTime = time(NULL)), (difftime(nowTime, beginOperation) < MAX_CLOUD_TRAVEL_TIME) //time box
        )
    {
        if (Lock(receiveUserContext->lock) != LOCK_OK)
        {
            ASSERT_FAIL("unable to lock");
        }
        else
        {
            if (receiveUserContext->wasFound)
            {
                (void)Unlock(receiveUserContext->lock);
                break;
            }
            (void)Unlock(receiveUserContext->lock);
        }
        ThreadAPI_Sleep(100);
    }

}

static void recv_message_test(IOTHUB_PROVISIONED_DEVICE* deviceToUse, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    // arrange
    IOTHUB_SERVICE_CLIENT_AUTH_HANDLE iotHubServiceClientHandle;
    IOTHUB_MESSAGING_CLIENT_HANDLE iotHubMessagingHandle;
    IOTHUB_CLIENT_HANDLE iotHubClientHandle;

    IOTHUB_MESSAGING_RESULT iotHubMessagingResult;
    IOTHUB_CLIENT_RESULT result;

    EXPECTED_RECEIVE_DATA* receiveUserContext;

    // Create device client
    iotHubClientHandle = client_connect_to_hub(deviceToUse, protocol);

    // Create receive context
    receiveUserContext = service_create_c2d(MSG_CONTENT);

    // Set callback
    result = IoTHubClient_SetMessageCallback(iotHubClientHandle, ReceiveMessageCallback, receiveUserContext);
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Setting message callback failed");

    // Create Service Client
    iotHubServiceClientHandle = IoTHubServiceClientAuth_CreateFromConnectionString(IoTHubAccount_GetIoTHubConnString(g_iothubAcctInfo));
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubServiceClientHandle, "Could not initialize IoTHubServiceClient to send C2D messages to the device");

    iotHubMessagingHandle = IoTHubMessaging_Create(iotHubServiceClientHandle);
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubMessagingHandle, "Could not initialize IoTHubMessaging to send C2D messages to the device");

    iotHubMessagingResult = IoTHubMessaging_Open(iotHubMessagingHandle, openCompleteCallback, (void*)"Context string for open");
    ASSERT_ARE_EQUAL (int, IOTHUB_MESSAGING_OK, iotHubMessagingResult);

    // Send message
    service_send_c2d(iotHubMessagingHandle, receiveUserContext, deviceToUse);

    // wait for message to arrive on client
    client_wait_for_c2d_event_arrival(receiveUserContext);

    // assert
    ASSERT_IS_TRUE_WITH_MSG(receiveUserContext->wasFound, "Failure retrieving data from C2D"); // was found is written by the callback...

    // cleanup
    IoTHubMessaging_Close(iotHubMessagingHandle);
    IoTHubMessaging_Destroy(iotHubMessagingHandle);
    IoTHubServiceClientAuth_Destroy(iotHubServiceClientHandle);

    IoTHubClient_Destroy(iotHubClientHandle);
    ReceiveUserContext_Destroy(receiveUserContext);
}
void e2e_recv_message_test_sas(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    recv_message_test(IoTHubAccount_GetSASDevice(g_iothubAcctInfo), protocol);
}

void e2e_recv_message_test_x509(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    recv_message_test(IoTHubAccount_GetX509Device(g_iothubAcctInfo), protocol);
}

