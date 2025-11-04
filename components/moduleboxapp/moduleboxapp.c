// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <axstring.h>
#include <mbdebug.h>
#include <moduleboxapp.h>
#include <stateConfig.h>
#include <string.h>
#include <mbdebug.h>

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define	REQ_NONE			0
#define	REQ_IN_PROGRESS		1
#define	REQ_NAME			2
#define	REQ_CONFIG			3

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------


// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern configuration 		me_config;

static  SemaphoreHandle_t   mbaMutex    		= NULL;

static 	int 				currentRequest		= REQ_NONE;
static 	char * 				currentBuffer 		= nil;
static 	int 				currentAvail		= 0;

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

void moduleboxapp_init()
{
    mbaMutex = xSemaphoreCreateMutex();
}
char * execGetName(int count)
{
	char * 	result 		= nil;

	if (count)
	{
		SAFE_FREE(currentBuffer);
	}
	else
	{
		SAFE_FREE(currentBuffer);

		if ((currentBuffer = _getAvailableBuff(&currentAvail)) != nil)
		{
			currentRequest = REQ_NAME;

			snprintf(currentBuffer, currentAvail, "%smyNameIs %s\n", MODULEBOXAPP_TOPIC, me_config.deviceName);
			result = currentBuffer;
		}		
	}

	return result;
}
char * moduleboxapp_command(char * command, int count)
{
	char * 	result 		= nil;
	char * 	on;
	UINT 	len;

//	printf("moduleboxapp_command: '%s' count = %d\n", command, count);

	if (count)
	{
//		printf("moduleboxapp_command: stage 1\n");
	}
	else // First call
	{ 
		bool 	got 		= false;

//		printf("moduleboxapp_command: stage 2\n");

		if (xSemaphoreTake(mbaMutex, portMAX_DELAY) == pdTRUE)
		{
			if (REQ_NONE == currentRequest)
			{
//				printf("moduleboxapp_command: stage 3\n");

				currentRequest 	= REQ_IN_PROGRESS;
				got 			= true;
			}

			xSemaphoreGive(mbaMutex);
		}

		if (got)
		{
//			printf("moduleboxapp_command: stage 4\n");

			on 	= strz_clean(command);
			len	= strlen(on);

			if ((on = strz_substrs_get_u(on, &len, ' ')) != nil)
			{
				if (!strcmp(on, "getName"))
				{
					result = execGetName(count);
				}
			}

			currentRequest 	= REQ_NONE;
		}
	}

	// if (result)
	// 	printf("RESPONSE: %s", result);

	return result;
}