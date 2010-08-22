#include <stdio.h>
#include <stdlib.h>

#include <gnokii.h>

/* prepare for i18n */
#define _(x) x // TODO test and search about this

// TODO test multipart sms
// TODO implement delete-on-report
// TODO implement alarm-on-report
// TODO implement basic sanity checks
//
// TODO transformar o void callbackdata em tx_data e tx_state para evitar passar os params como globais

struct gn_statemachine *rx_state2 = NULL;
int running = 1;

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
    busterminate(rx_state2);
 //   busterminate(tx_state);
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

static gn_error sendsms(gn_statemachine *state, gn_data *data, char* msg, char* srcnumber, const char* destnumber)
{

    gn_sms sms;
    gn_error error;

    /* The maximum length of an uncompressed concatenated short message is
	   255 * 153 = 39015 default alphabet characters => as in gnokii-sms.c */ 

    
    gn_sms_default_submit(&sms);

    snprintf(sms.remote.number, sizeof(sms.remote.number), "%s", destnumber);
    sms.remote.type = get_number_type(sms.remote.number);
	if (sms.remote.type == GN_GSM_NUMBER_Alphanumeric) {
		fprintf(stderr, _("Invalid phone number\n"));
		return GN_ERR_WRONGDATAFORMAT;
	}

    sms.delivery_report = true;
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
    fprintf(stdout, "LOL pint\n");
    char *s = message->user_data[0].u.text;

    char number[GN_BCD_STRING_MAX_LENGTH];
    char *p = message->remote.number;

    int i = message->number;
    
    snprintf(number, sizeof(number), "%s", p);
    fprintf(stderr, _("SMS received from number: %s\n"), number);

    fprintf(stderr, _("Got message %d: %s\n"), i, s);

    //TODO log msg

    // send msg
    if ((error = sendsms(tx_state, tx_data, srcnumber, destnumber) != GN_ERR_NONE)) {
            fprintf(stderr, _("Error sending the message\n"));
            return error;
    }

    //TODO delete msg

    return GN_ERR_NONE;
}
                

int main(int argc, char *argv[])
{
    struct gn_statemachine *rx_state = NULL;
    struct gn_statemachine *tx_state = NULL;

    gn_data rx_data;
    gn_data tx_data;

    gn_error error;
    
    if (argc != 3) {
        fprintf(stderr, _("Usage: %s <receiver_configfile> <transmiter configfile>\n"), argv[0]);
        return 1;
    }

    fprintf(stdout, "1: %s\n 2: %s\n", argv[1] ,argv[2]);

    businit(argv[1], NULL, &rx_state, &rx_data);
//    businit(argv[2], NULL, tx_state, &tx_data);
    
    atexit(terminate_rxtx);

    rx_state2 = rx_state;

    //gn_data_clear(&rx_data);
    //rx_data = rx_state->sm_data;

    rx_data.on_sms = &handlesms;
    rx_data.callback_data = NULL;
    error = gn_sm_functions(GN_OP_OnSMS, &rx_data, rx_state);

    signal(SIGINT, interrupted);

    while (running)
        gn_sm_loop(1, rx_state);

    fprintf(stdout, _("terminated\n"));

    return 0;
}


