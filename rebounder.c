#include <stdio.h>
#include <stdlib.h>

#include <gnokii.h>

/* prepare for i18n */
#define _(x) x // TODO test and search about this

// TODO test multipart sms
// TODO implement delete-on-report
// TODO implement alarm-on-report
// TODO implement basic sanity checks

struct gn_statemachine *rx_state = NULL;
struct gn_statemachine *tx_state = NULL;
int running = 1;

struct tx_pack {
    char* destnumber;
    gn_data *data;
    struct gn_statemachine *state;
};

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
    busterminate(tx_state);
}

static int businit(const char *configfile, const char *configmodel, struct gn_statemachine **state, gn_data *data)
{
    gn_error err;

    if ((err = gn_lib_phoneprofile_load_from_file(configfile, configmodel, state)) != GN_ERR_NONE) {
        fprintf(stderr, "lol %s\n", gn_error_print(err));
        if (configfile)
            fprintf(stderr, _("File: %s\n"), configfile);
        if (configmodel)
            fprintf(stderr, _("Phone section: [phone_%s]\n"), configmodel);
        return 2;
    }

    if ((err = gn_lib_phone_open(*state)) != GN_ERR_NONE) {
        fprintf(stderr, "bode2 %s\n", gn_error_print(err));
        return 2;
    }
   data = &((**state).sm_data);

    return 0;
}

static gn_error sendsms(struct gn_statemachine *state, gn_data *data, char* msg, char* srcnumber, char *destnumber)
{

    gn_sms sms;
    gn_error error;

    /* The maximum length of an uncompressed concatenated short message is
	   255 * 153 = 39015 default alphabet characters => as in gnokii-sms.c */ 

    
    gn_sms_default_submit(&sms);

    snprintf(sms.remote.number, sizeof(sms.remote.number), "%s", destnumber);
    sms.remote.type = GN_GSM_NUMBER_Unknown;
	if (sms.remote.type == GN_GSM_NUMBER_Alphanumeric) {
		fprintf(stderr, _("Invalid phone number\n"));
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
			fprintf(stderr, _("Cannot read the SMSC number from your phone. If the sms send will fail, please use --smsc option explicitely giving the number.\n"));
		}
		free(data->message_center);
	}

    if (!sms.smsc.type) sms.smsc.type = GN_GSM_NUMBER_Unknown;

    sms.user_data[0].type = GN_SMS_DATA_Text;

    if(!gn_char_def_alphabet(sms.user_data[0].u.text))
        sms.dcs.u.general.alphabet = GN_SMS_DCS_UCS2;

    data->sms = &sms;

    error = gn_sms_send(data, state);
    
    if (error == GN_ERR_NONE) {
		if (sms.parts > 1) {
			int j;
			fprintf(stderr, _("Message sent in %d parts with reference numbers:"), sms.parts);
			for (j = 0; j < sms.parts; j++)
				fprintf(stderr, " %d", sms.reference[j]);
			fprintf(stderr, "\n");
		} else
			fprintf(stderr, _("Send succeeded with reference %d!\n"), sms.reference[0]);
	} else
		fprintf(stderr, _("SMS Send failed (%s)\n"), gn_error_print(error));

	free(sms.reference);


    return error;
}


static gn_error handlesms(gn_sms *message, struct gn_statemachine *state, void *callbackdata)
{
    gn_error error;
    fprintf(stdout, "LOL pint\n");

    // get the callbackdata
    struct tx_pack* _callbackdata = (struct tx_pack*)callbackdata;
    struct gn_statemachine *_tx_state = (struct gn_statemachine*)_callbackdata->state;
    gn_data *tx_data = _callbackdata->data;
    char* destnumber = _callbackdata->destnumber;

    char *msg = message->user_data[0].u.text;

    char srcnumber[GN_BCD_STRING_MAX_LENGTH];
    char *p = message->remote.number;

    int i = message->number;
    
    snprintf(srcnumber, sizeof(srcnumber), "%s", p);
    fprintf(stderr, _("SMS received from number: %s\n"), srcnumber);

    fprintf(stderr, _("Got message %d: %s\n"), i, msg);

    //TODO log msg

    // send msg
    if ((error = sendsms(_tx_state, tx_data, msg, srcnumber, destnumber) != GN_ERR_NONE)) {
            fprintf(stderr, _("Error sending the message\n"));
            return error;
    }

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
    
    if (argc != 4) {
        fprintf(stderr, _("Usage: %s <receiver_configfile> <transmiter configfile> <destination number>\n"), argv[0]);
        return 1;
    }

    fprintf(stdout, "1: %s\n 2: %s\n", argv[1] ,argv[2]);

    //TODO verify the number format
    destnumber = argv[3];

    businit(argv[1], NULL, &rx_state, &rx_data);
//    businit(argv[2], NULL, tx_state, &tx_data);
    
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

    fprintf(stdout, _("terminated\n"));

    return 0;
}


