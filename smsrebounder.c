#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <gnokii.h>

#define DEBUG 3
#define EVENT 2
#define ERROR 1

// TODO test multipart sms
// TODO implement delete-on-report
// TODO implement alarm-on-report
// TODO implement basic sanity checks

struct gn_statemachine *rx_state = NULL;
struct gn_statemachine *tx_state = NULL;
int loglevel = DEBUG;
FILE* logfile;
int running = 1;

struct tx_pack {
    char* destnumber;
    gn_data *data;
    struct gn_statemachine *state;
};

void logprintf(int level, const char *__restrict format, ...)
{
    if (loglevel < level) {
        return;
    }

    //timestamp
    time_t _time;
    struct tm* ltime;
    char timestamp[30];
    _time = time(NULL);
    ltime = localtime(&_time);
    sprintf(timestamp, "[%d/%d/%d - %d:%d:%d] ", ltime->tm_mday,
                                                 ltime->tm_mon,
                                                 ltime->tm_year,
                                                 ltime->tm_hour,
                                                 ltime->tm_min,
                                                 ltime->tm_sec);

    fprintf(logfile, "%s", timestamp);
    va_list args;
    va_start(args, format);
    vfprintf(logfile, format, args);
    va_end(args);
    fprintf(logfile, "\n");

    if (level == DEBUG)
        fflush(logfile);

}

void interrupted(int sig)
{
	signal(sig, SIG_IGN);
    running = 0;
}

void busterminate(struct gn_statemachine *state)
{
    gn_lib_phone_close(state);
    gn_lib_phoneprofile_free(&state);
    gn_lib_library_free();
}

void terminate_rxtx(void)
{
    busterminate(rx_state);
    logprintf(DEBUG, "Terminated RX device");
    //busterminate(tx_state);
    //logprintf(DEBUG, "Terminated TX device");
}

static int businit(const char *configfile, const char *configmodel, struct gn_statemachine **state, gn_data *data)
{
    logprintf(DEBUG, "Initiating device using config at %s", configfile);
    gn_error err;

    if ((err = gn_lib_phoneprofile_load_from_file(configfile, configmodel, state)) != GN_ERR_NONE) {
        logprintf(ERROR, "Error reading device configfile: %s", gn_error_print(err));
        if (configfile)
            logprintf(DEBUG, "Config file: %s", configfile);
        if (configmodel)
            logprintf(DEBUG, "Config model: %s", configmodel);
        return 2;
    }

    if ((err = gn_lib_phone_open(*state)) != GN_ERR_NONE) {
        logprintf(ERROR, "Error initializing device: %s", gn_error_print(err));
        return 2;
    }
   //data = &((*state)->sm_data);

    return 0;
}

static gn_error sendsms(struct gn_statemachine *state, gn_data *data, char* msg, char* srcnumber, char *destnumber)
{

    gn_sms sms;
    gn_error error;
    int retries = 5;

    /* The maximum length of an uncompressed concatenated short message is
	   255 * 153 = 39015 default alphabet characters => as in gnokii-sms.c */ 

    
    gn_sms_default_submit(&sms);

    snprintf(sms.remote.number, sizeof(sms.remote.number), "%s", destnumber);
    sms.remote.type = GN_GSM_NUMBER_Unknown;
	if (sms.remote.type == GN_GSM_NUMBER_Alphanumeric) {
		return GN_ERR_WRONGDATAFORMAT;
	}

    sms.delivery_report = 1;
    sms.validity = 11520; /* 1 week */

    /* get smsc number */
    if (!sms.smsc.number[0]) {
		data->message_center = calloc(1, sizeof(gn_sms_message_center));
		data->message_center->id = 1;
		if (gn_sm_functions(GN_OP_GetSMSCenter, data, state) == GN_ERR_NONE) {
			snprintf(sms.smsc.number, sizeof(sms.smsc.number), "%s", data->message_center->smsc.number);
			sms.smsc.type = data->message_center->smsc.type;
		} else {
            logprintf(DEBUG, "Cannot read the SMSC number from the device.");
		}
		free(data->message_center);
	}

    if (!sms.smsc.type) sms.smsc.type = GN_GSM_NUMBER_Unknown;

    sms.user_data[0].type = GN_SMS_DATA_Text;

    if(!gn_char_def_alphabet(sms.user_data[0].u.text))
        sms.dcs.u.general.alphabet = GN_SMS_DCS_UCS2;

    data->sms = &sms;

    while((error = gn_sms_send(data, state)) != GN_ERR_NONE) {
        logprintf(ERROR, "Error sending message: %s - retries left: %d", gn_error_print(error), retries--);
        // sleep between retires
        sleep(5);
    }
    
    if (error == GN_ERR_NONE) {
		if (sms.parts > 1) {
			int j;
            char references[sms.parts*10];
			sprintf(references, "Message sent in %d parts with reference numbers:", sms.parts);
			for (j = 0; j < sms.parts; j++)
				sprintf(references, " %d", sms.reference[j]);

            logprintf(DEBUG, "Sent message in %d parts: %s", sms.parts, references);
		} else
            logprintf(DEBUG, "Sent message in 1 part: %s", sms.reference[0]);
        logprintf(EVENT, "Message sent to %d - %s", destnumber, msg);
	}

	free(sms.reference);

    return error;
}


static gn_error handlesms(gn_sms *message, struct gn_statemachine *state, void *callbackdata)
{
    logprintf(DEBUG, "New message!");
    gn_error error;

    // get the callbackdata
    struct tx_pack* _callbackdata = (struct tx_pack*)callbackdata;
    struct gn_statemachine *_tx_state = (struct gn_statemachine*)_callbackdata->state;
    gn_data *tx_data = _callbackdata->data;
    char* destnumber = _callbackdata->destnumber;

    char *msg = message->user_data[0].u.text;

    char srcnumber[GN_BCD_STRING_MAX_LENGTH];
    char *p = message->remote.number;

    int i = message->number;
    
    logprintf(DEBUG, "printing srcnumber!");
    snprintf(srcnumber, sizeof(srcnumber), "%s", p);

    logprintf(DEBUG, "Got message on slot %d", i);
    logprintf(EVENT, "Message received from %s - %s", srcnumber, msg);

    // send msg
    /*
    if ((error = sendsms(_tx_state, tx_data, msg, srcnumber, destnumber) != GN_ERR_NONE)) {
        logprintf(ERROR, "Error sending message on slot %d: %s", i, msg);
        // TODO message sending failed, should send by email or any other extreme method
    }
*/
    //TODO delete msg

    return GN_ERR_NONE;
}
                

int main(int argc, char *argv[])
{

    gn_data rx_data;
    gn_data tx_data;

    gn_error error;
    char* destnumber;
    struct tx_pack* callback_pack = (struct tx_pack*)malloc(sizeof(struct tx_pack*));
    
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "Usage: %s <receiver_configfile> <transmiter configfile> <destination number> [/path/to/logfile]\n", argv[0]);
        return 1;
    }

    //TODO verify the number format
    destnumber = argv[3];
    
    // TODO check if logfile exists and is writable
    // TODO use a default logfile
    logfile = fopen(argv[4], "a+");
    loglevel = DEBUG;

    logprintf(DEBUG, "Initiating RX device");
    businit(argv[1], NULL, &rx_state, &rx_data);
    rx_data = rx_state->sm_data;

    logprintf(DEBUG, "Phone model: %s", (rx_state->config.model));
    logprintf(DEBUG, "Phone model: %d", (rx_state->sm_data));
    //logprintf(DEBUG, "Initiating TX device");
    //businit(argv[2], NULL, &tx_state, &tx_data);
    
    atexit(terminate_rxtx);

    //gn_data_clear(&rx_data);
    //rx_data = rx_state->sm_data;

    rx_data.on_sms = &handlesms;
    callback_pack->destnumber = destnumber;
    callback_pack->state = tx_state;
    callback_pack->data = &tx_data;
    rx_data.callback_data = (void*)callback_pack;
    error = gn_sm_functions(GN_OP_OnSMS, &rx_data, rx_state);

    signal(SIGINT, interrupted);

    while (running)
        gn_sm_loop(1, rx_state);

    fclose(logfile);

    logprintf(EVENT, "Program terminated.");

    return 0;
}

