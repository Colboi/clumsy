// bandwidth cap module
#include <stdlib.h>
#include <Windows.h>
#include <stdint.h>

#include "iup.h"
#include "common.h"

#define NAME "bandwidth"
#define BANDWIDTH_MIN  "0"
#define BANDWIDTH_MAX  "99999"
#define BANDWIDTH_DEFAULT 10

typedef struct {
    int bandwidth;  // in KB/s
    int duration;  // in milliseconds
} BandwidthConfig;

static BandwidthConfig *bandwidthConfigs = NULL;
static int configCount = 0;
static int currentConfigIndex = 0;
static DWORD lastConfigChangeTime = 0;

//---------------------------------------------------------------------
// log file
//---------------------------------------------------------------------

#define LOG_FILE "log.txt"

//---------------------------------------------------------------------
// rate stats
//---------------------------------------------------------------------
typedef struct {
	int32_t initialized;
	uint32_t oldest_index;
	uint32_t oldest_ts;
	int64_t accumulated_count;
	int32_t sample_num;
	int window_size;
	float scale;
	uint32_t *array_sum;
	uint32_t *array_sample;
} CRateStats;


CRateStats* crate_stats_new(int window_size, float scale);

void crate_stats_delete(CRateStats *rate);

void crate_stats_reset(CRateStats *rate);

// call when packet arrives, count is the packet size in bytes
void crate_stats_update(CRateStats *rate, int32_t count, uint32_t now_ts);

// calculate rate
int32_t crate_stats_calculate(CRateStats *rate, uint32_t now_ts);


//---------------------------------------------------------------------
// configuration
//---------------------------------------------------------------------
static Ihandle *inboundCheckbox, *outboundCheckbox, *bandwidthInput, *currentBandwidthLabel, *useConfigCheckbox;

static volatile short bandwidthEnabled = 0,
    bandwidthInbound = 1, bandwidthOutbound = 1, useConfig = 1;

static volatile LONG bandwidthLimit = BANDWIDTH_DEFAULT; 
static CRateStats *rateStats = NULL;

static void readBandwidthConfig() {
    FILE *file = fopen("bandwidth.txt", "r");
    if (file == NULL) {
		// write to log file
		FILE *logFile = fopen(LOG_FILE, "a");
		if (logFile) {
			fprintf(logFile, "Failed to open bandwidth.txt in this directory: %s\n", getcwd(NULL, 0));
			fclose(logFile);
		}
        return;
    }

    int lineCount = 0;
    char line[100];
    while (fgets(line, sizeof(line), file) != NULL) {
        lineCount++;
    }
    rewind(file);

    bandwidthConfigs = (BandwidthConfig *)malloc(lineCount * sizeof(BandwidthConfig));
    if (bandwidthConfigs == NULL) {
        LOG("Failed to allocate memory for bandwidth configs");
        fclose(file);
        return;
    }

    configCount = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        int bandwidth, duration;
        if (sscanf(line, "%d %d", &bandwidth, &duration) == 2) {
            bandwidthConfigs[configCount].bandwidth = bandwidth / 8;
            bandwidthConfigs[configCount].duration = duration;
            configCount++;
        }
    }

    fclose(file);
}

static int onUseConfigToggled(Ihandle *self)
{
    int useCfg = IupGetInt(self, "VALUE");
	useConfig = useCfg;
    IupSetAttribute(bandwidthInput, "ACTIVE", useCfg ? "NO" : "YES");
    return IUP_DEFAULT;
}

static Ihandle* bandwidthSetupUI() {
    Ihandle *bandwidthControlsBox = IupHbox(
		currentBandwidthLabel = IupLabel("N/A"),
		useConfigCheckbox = IupToggle("Use config", NULL),
        inboundCheckbox = IupToggle("Inbound", NULL),
        outboundCheckbox = IupToggle("Outbound", NULL),
        IupLabel("Limit(kB/s):"),
        bandwidthInput = IupText(NULL),
        NULL
    );

	IupSetAttribute(currentBandwidthLabel, "RASTERSIZE", "150x20");
    IupSetAttribute(bandwidthInput, "VISIBLECOLUMNS", "4");
    IupSetAttribute(bandwidthInput, "VALUE", STR(BANDWIDTH_DEFAULT));
    IupSetCallback(bandwidthInput, "VALUECHANGED_CB", uiSyncInt32);
    IupSetAttribute(bandwidthInput, SYNCED_VALUE, (char*)&bandwidthLimit);
    IupSetAttribute(bandwidthInput, INTEGER_MAX, BANDWIDTH_MAX);
    IupSetAttribute(bandwidthInput, INTEGER_MIN, BANDWIDTH_MIN);
    IupSetCallback(inboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(inboundCheckbox, SYNCED_VALUE, (char*)&bandwidthInbound);
    IupSetCallback(outboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(outboundCheckbox, SYNCED_VALUE, (char*)&bandwidthOutbound);
	IupSetCallback(useConfigCheckbox, "ACTION", (Icallback)onUseConfigToggled);
	IupSetAttribute(useConfigCheckbox, SYNCED_VALUE, (char*)&useConfig);
	IupSetAttribute(bandwidthInput, "ACTIVE", useConfig ? "NO" : "YES");

    // enable by default to avoid confusing
    IupSetAttribute(inboundCheckbox, "VALUE", "ON");
    IupSetAttribute(outboundCheckbox, "VALUE", "ON");
	IupSetAttribute(useConfigCheckbox, "VALUE", "ON");

    if (parameterized) {
        setFromParameter(inboundCheckbox, "VALUE", NAME"-inbound");
        setFromParameter(outboundCheckbox, "VALUE", NAME"-outbound");
        setFromParameter(bandwidthInput, "VALUE", NAME"-bandwidth");
        setFromParameter(useConfigCheckbox, "VALUE", NAME"-useconfig");
    }

    return bandwidthControlsBox;
}

static void bandwidthStartUp() {
	// ensure log file is created
	FILE *logFile = fopen(LOG_FILE, "a");
	if (logFile) fclose(logFile);

	if (rateStats) crate_stats_delete(rateStats);
	rateStats = crate_stats_new(1000, 1000);
    readBandwidthConfig();
    currentConfigIndex = 0;
    lastConfigChangeTime = timeGetTime();
    if (useConfig && configCount > 0) {
        bandwidthLimit = bandwidthConfigs[0].bandwidth;
    } else {
		bandwidthLimit = atol(IupGetAttribute(bandwidthInput, "VALUE"));
	}
    if (configCount > 0 || !useConfig) {
        char labelText[50];
        sprintf(labelText, "%dkbps", bandwidthLimit * 8);
        IupSetAttribute(currentBandwidthLabel, "TITLE", labelText);
		IupRefresh(currentBandwidthLabel);
    }
    LOG("bandwidth enabled");
}

static void bandwidthCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(head);
    UNREFERENCED_PARAMETER(tail);
	if (rateStats) crate_stats_delete(rateStats);
	rateStats = NULL;
    if (bandwidthConfigs) {
        free(bandwidthConfigs);
        bandwidthConfigs = NULL;
        configCount = 0;
    }
    LOG("bandwidth disabled");
}


//---------------------------------------------------------------------
// process
//---------------------------------------------------------------------
static short bandwidthProcess(PacketNode *head, PacketNode* tail) {
    int dropped = 0;
	DWORD now_ts = timeGetTime();

    if (useConfig && configCount > 0) {
        DWORD elapsedTime = now_ts - lastConfigChangeTime;
        if (elapsedTime >= bandwidthConfigs[currentConfigIndex].duration) {
            currentConfigIndex = (currentConfigIndex + 1) % configCount;
            bandwidthLimit = bandwidthConfigs[currentConfigIndex].bandwidth;
            lastConfigChangeTime = now_ts;
            LOG("Switched to bandwidth %dkB/s for %dms", bandwidthLimit, bandwidthConfigs[currentConfigIndex].duration);
            char labelText[50];
            sprintf(labelText, "%dkbps", bandwidthLimit * 8);
            IupSetAttribute(currentBandwidthLabel, "TITLE", labelText);
			IupRefresh(currentBandwidthLabel);
        }
    } else {
        bandwidthLimit = atol(IupGetAttribute(bandwidthInput, "VALUE"));
        char labelText[50];
        sprintf(labelText, "%dkbps", bandwidthLimit * 8);
        IupSetAttribute(currentBandwidthLabel, "TITLE", labelText);
		IupRefresh(currentBandwidthLabel);
    }

	int limit = bandwidthLimit * 1000;

	//	allow 0 limit which should drop all
	if (limit < 0 || rateStats == NULL) {
		return 0;
	}

    while (head->next != tail) {
        PacketNode *pac = head->next;
		int discard = 0;
        // chance in range of [0, 10000]
        if (checkDirection(pac->addr.Outbound, bandwidthInbound, bandwidthOutbound)) {
			int rate = crate_stats_calculate(rateStats, now_ts);
			int size = pac->packetLen;
			// write to log file
			FILE *logFile = fopen(LOG_FILE, "a");
			if (logFile) {
				fprintf(logFile, "Calc Rate: %d B/s, New packet size: %d bytes, Limit: %d B/s\n", rate, size, limit);
				fclose(logFile);
			}
			if (rate + size > limit) {
				LOG("dropped with bandwidth %dkB/s, direction %s",
					(int)bandwidthLimit, pac->addr.Outbound ? "OUTBOUND" : "INBOUND");
				discard = 1;
			}
			else {
				crate_stats_update(rateStats, size, now_ts);
			}
		}
		if (discard) {
            freeNode(popNode(pac));
            ++dropped;
        } else {
            head = head->next;
        }
    }
    return dropped > 0;
}


//---------------------------------------------------------------------
// module
//---------------------------------------------------------------------
Module bandwidthModule = {
    "Bandwidth",
    NAME,
    (short*)&bandwidthEnabled,
    bandwidthSetupUI,
    bandwidthStartUp,
    bandwidthCloseDown,
    bandwidthProcess,
    // runtime fields
    0, 0, NULL
};



//---------------------------------------------------------------------
// create new CRateStat
//---------------------------------------------------------------------
CRateStats* crate_stats_new(int window_size, float scale)
{
	CRateStats *rate = (CRateStats*)malloc(sizeof(CRateStats));
	assert(rate);
	rate->array_sum = (uint32_t*)malloc(sizeof(uint32_t) * window_size);
	assert(rate->array_sum);
	rate->array_sample = (uint32_t*)malloc(sizeof(uint32_t) * window_size);
	assert(rate->array_sample);
	rate->window_size = window_size;
	rate->scale = scale;
	crate_stats_reset(rate);
	return rate;
}


//---------------------------------------------------------------------
// delete rate
//---------------------------------------------------------------------
void crate_stats_delete(CRateStats *rate)
{
	if (rate) {
		rate->window_size = 0;
		if (rate->array_sum) free(rate->array_sum);
		if (rate->array_sample) free(rate->array_sample);
		rate->array_sum = NULL;
		rate->array_sample = NULL;
		rate->initialized = 0;
		free(rate);
	}
}


//---------------------------------------------------------------------
// reset rate
//---------------------------------------------------------------------
void crate_stats_reset(CRateStats *rate)
{
	int i;
	for (i = 0; i < rate->window_size; i++) {
		rate->array_sum[i] = 0;
		rate->array_sample[i] = 0;
	}
	rate->initialized = 0;
	rate->sample_num = 0;
	rate->accumulated_count = 0;
	rate->oldest_ts = 0;
	rate->oldest_index = 0;
}


//---------------------------------------------------------------------
// evict oldest history
//---------------------------------------------------------------------
void crate_stats_evict(CRateStats *rate, uint32_t now_ts)
{
	if (rate->initialized == 0) 
		return;

	uint32_t new_oldest_ts = now_ts - ((uint32_t)rate->window_size) + 1;

	if (((int32_t)(new_oldest_ts - rate->oldest_ts)) < 0) 
		return;

	while (((int32_t)(rate->oldest_ts - new_oldest_ts)) < 0) {
		uint32_t index = rate->oldest_index;
		if (rate->sample_num == 0) break;
		rate->sample_num -= rate->array_sample[index];
		rate->accumulated_count -= rate->array_sum[index];
		rate->array_sample[index] = 0;
		rate->array_sum[index] = 0;
		rate->oldest_index++;
		if (rate->oldest_index >= (uint32_t)rate->window_size) {
			rate->oldest_index = 0;
		}
		rate->oldest_ts++;
	}
	assert(rate->sample_num >= 0);
	assert(rate->accumulated_count >= 0);
	rate->oldest_ts = new_oldest_ts;
}


//---------------------------------------------------------------------
// update stats
//---------------------------------------------------------------------
void crate_stats_update(CRateStats *rate, int32_t count, uint32_t now_ts)
{
	if (rate->initialized == 0) {
		rate->oldest_ts = now_ts;
		rate->oldest_index = 0;
		rate->accumulated_count = 0;
		rate->sample_num = 0;
		rate->initialized = 1;
	}

	if (((int32_t)(now_ts - rate->oldest_ts)) < 0) {
		return;
	}

	crate_stats_evict(rate, now_ts);

	int32_t offset = (int32_t)(now_ts - rate->oldest_ts);
	int32_t index = (rate->oldest_index + offset) % rate->window_size;

	rate->sample_num++;
	rate->accumulated_count += count;
	rate->array_sum[index] += count;
	rate->array_sample[index] += 1;
}


//---------------------------------------------------------------------
// calculate
//---------------------------------------------------------------------
int32_t crate_stats_calculate(CRateStats *rate, uint32_t now_ts)
{
	int32_t active_size = (int32_t)(now_ts - rate->oldest_ts + 1);
	float r;

	crate_stats_evict(rate, now_ts);

	if (rate->initialized == 0 || 
		rate->sample_num <= 0 || 
		active_size <= 1 || 
		active_size < rate->window_size) {
		return -1;
	}

	r = ((((float)rate->accumulated_count) * rate->scale) / 
				rate->window_size) + 0.5f;

	return (int32_t)r;
}



