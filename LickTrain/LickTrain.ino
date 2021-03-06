/* Lick train protocol

Reward licks on one side for a fixed number of rewards, then switch.
*/
#include "chat.h"
#include "hwconstants.h"
#include "mpr121.h"
#include <Wire.h> // also for mpr121
#include "TimedState.h"
#include "States.h"


#define FAKE_RESPONDER 0

extern char* param_abbrevs[N_TRIAL_PARAMS];
extern long param_values[N_TRIAL_PARAMS];
extern bool param_report_ET[N_TRIAL_PARAMS];
extern char* results_abbrevs[N_TRIAL_RESULTS];
extern long results_values[N_TRIAL_RESULTS];
extern long default_results_values[N_TRIAL_RESULTS];

//// Miscellaneous globals
// flag to remember whether we've received the start next trial signal
// currently being used in both setup() and loop() so it can't be staticked
bool flag_start_trial = 0;


//// Declarations
int take_action(char *protocol_cmd, char *argument1, char *argument2);


//// User-defined variables, etc, go here
/// these should all be staticked into loop()
unsigned int rewards_this_trial = 0;
STATE_TYPE next_state; 

// touched monitor
uint16_t sticky_touched = 0;


//// Setup function
void setup()
{
  unsigned long time = millis();
  int status = 1;
  
  Serial.begin(115200);
  Serial.print(time);
  Serial.println(" DBG begin setup");

  //// Begin user protocol code
  //// Put this in a user_setup1() function?
  
  // MPR121 touch sensor setup
  pinMode(TOUCH_IRQ, INPUT);
  digitalWrite(TOUCH_IRQ, HIGH); //enable pullup resistor
  Wire.begin();
  
  // output pins
  pinMode(L_REWARD_VALVE, OUTPUT);
  pinMode(R_REWARD_VALVE, OUTPUT);
  pinMode(__HWCONSTANTS_H_HOUSE_LIGHT, OUTPUT);
  
  // initialize the house light to OFF
  digitalWrite(__HWCONSTANTS_H_HOUSE_LIGHT, LOW);
  
  // random number seed
  randomSeed(analogRead(3));
  
  //// Run communications until we've received all setup info
  // Later make this a new flag. For now wait for first trial release.
  while (!flag_start_trial)
  {
    status = communications(time);
    if (status != 0)
    {
      Serial.println("comm error in setup");
      delay(1000);
    }
  }
  
  //// Now finalize the setup using the received initial parameters
  // user_setup2() function?
  
  // thresholds for MPR121
  mpr121_setup(TOUCH_IRQ, param_values[tpidx_TOU_THRESH], 
    param_values[tpidx_REL_THRESH]);
}



//// Loop function
void loop()
{ /* Called over and over again. On each call, the behavior is determined
     by the current state.
  */
  
  //// Variable declarations
  // get the current time as early as possible in this function
  unsigned long time = millis();

  // statics 
  // these are just "declared" here, they can be modified at the beginning
  // of each trial
  static STATE_TYPE current_state = WAIT_TO_START_TRIAL;
  static StateResponseWindow srw(param_values[tpidx_RESP_WIN_DUR]);
  static StateInterTrialInterval state_inter_trial_interval(
    param_values[tpidx_INTER_REWARD_INTERVAL]);
  static StatePostRewardPause state_post_reward_pause(
        param_values[tpidx_INTER_REWARD_INTERVAL]);

  // The next state, by default the same as the current state
  next_state = current_state;    
  
  // misc
  int status = 1;
  
  //// User protocol variables
  uint16_t touched = 0;
  
  
  //// Run communications
  status = communications(time);
  
  //// User protocol code
  // could put other user-specified every_loop() stuff here
  
  // Poll touch inputs
  touched = pollTouchInputs();
  
  // announce sticky
  if (touched != sticky_touched)
  {
    Serial.print(time);
    Serial.print(" TCH ");
    Serial.println(touched);
    sticky_touched = touched;
  }  
  
  //// Begin state-dependent operations
  // Try to replace every case with a single function or object call
  // Ultimately this could be a dispatch table.
  // Also, eventually we'll probably want them to return next_state,
  // but currently it's generally passed by reference.
  switch (current_state)
  {
    //// Wait till the trial is released. Same for all protocols.
    case WAIT_TO_START_TRIAL:
      // Wait until we receive permission to continue  
      if (flag_start_trial)
      {
        // Announce that we have ended the trial and reset the flag
        Serial.print(time);
        Serial.println(" TRL_RELEASED");
        flag_start_trial = 0;
        
        // Proceed to next trial
        next_state = TRIAL_START;
      }
      break;

    //// TRIAL_START. Same for all protocols.
    case TRIAL_START:
      // Set up the trial based on received trial parameters
      Serial.print(time);
      Serial.println(" TRL_START");
      for(int i=0; i < N_TRIAL_PARAMS; i++)
      {
        if (param_report_ET[i]) 
        {
          // Buffered write would be nice here
          Serial.print(time);
          Serial.print(" TRLP ");
          Serial.print(param_abbrevs[i]);
          Serial.print(" ");
          Serial.println(param_values[i]);
        }
      }
    
      // Set up trial_results to defaults
      for(int i=0; i < N_TRIAL_RESULTS; i++)
      {
        results_values[i] = default_results_values[i];
      }      
      
      
      //// User-defined code goes here
      // make this in state_first_state()?
      // declare the states. Here we're both updating the parameters
      // in case they've changed, and resetting all timers.
      srw = StateResponseWindow(param_values[tpidx_RESP_WIN_DUR]);
      state_inter_trial_interval = StateInterTrialInterval(
        param_values[tpidx_INTER_REWARD_INTERVAL]);      
      
      next_state = RESPONSE_WINDOW;
    
      break;
  
    case RESPONSE_WINDOW:
      srw.update(touched);
      srw.run(time);
      break;
    
    case REWARD_L:
      Serial.print(time);
      Serial.println(" EV R_L");
      state_reward_l(next_state);
      break;
    
    case REWARD_R:
      Serial.print(time);
      Serial.println(" EV R_R");      
      state_reward_r(next_state);
      break;
    
    case POST_REWARD_PAUSE:
      state_post_reward_pause.run(time);
      break;

    case INTER_TRIAL_INTERVAL:
      // Announce trial_results
      state_inter_trial_interval.run(time);
      break;
  }
  
  
  //// Update the state variable
  if (next_state != current_state)
  {
    Serial.print(time);
    Serial.print(" ST_CHG ");
    Serial.print(current_state);
    Serial.print(" ");
    Serial.println(next_state);
  }
  current_state = next_state;
  
  return;
}


//// Take protocol action based on user command (ie, setting variable)
int take_action(char *protocol_cmd, char *argument1, char *argument2)
{ /* Protocol action.
  
  Currently two possible actions:
    if protocol_cmd == 'SET':
      argument1 is the variable name. argument2 is the data.
    if protocol_cmd == 'ACT':
      argument1 is converted into a function based on a dispatch table.
        REWARD_L : reward the left valve
        REWARD_R : reward the right valve
        REWARD : reward the current valve

  This logic could be incorporated in TrialSpeak, but we would need to provide
  the abbreviation, full name, datatype, and optional handling logic for
  each possible variable. So it seems to make more sense here.
  
  Return values:
  0 - command parsed successfully
  2 - unimplemented protocol_cmd
  4 - unknown variable on SET command
  5 - data conversion error
  6 - unknown asynchronous action
  */
  int status;
  
  //~ Serial.print("DBG take_action ");
  //~ Serial.print(protocol_cmd);
  //~ Serial.print("-");
  //~ Serial.print(argument1);
  //~ Serial.print("-");
  //~ Serial.println(argument2);
  
  if (strncmp(protocol_cmd, "SET\0", 4) == 0)
  {
    // Find index into param_abbrevs
    int idx = -1;
    for (int i=0; i < N_TRIAL_PARAMS; i++)
    {
      if (strcmp(param_abbrevs[i], argument1) == 0)
      {
        idx = i;
        break;
      }
    }
    
    // Error if not found, otherwise set
    if (idx == -1)
    {
      Serial.print("ERR param not found ");
      Serial.println(argument1);
      return 4;
    }
    else
    {
      // Convert to int
      status = safe_int_convert(argument2, param_values[idx]);

      //~ // Debug
      //~ Serial.print("DBG setting var ");
      //~ Serial.print(idx);
      //~ Serial.print(" to ");
      //~ Serial.println(argument2);

      // Error report
      if (status != 0)
      {
        Serial.println("ERR can't set var");
        return 5;
      }
    }
  }   

  else if (strncmp(protocol_cmd, "ACT\0", 4) == 0)
  {
    // Dispatch
    if (strncmp(argument1, "REWARD_L\0", 9) == 0) {
      asynch_action_reward_l();
    } else if (strncmp(argument1, "REWARD_R\0", 9) == 0) {
      asynch_action_reward_r();
    } else if (strncmp(argument1, "REWARD\0", 7) == 0) {
      asynch_action_reward();
    } else if (strncmp(argument1, "THRESH\0", 7) == 0) {
      asynch_action_set_thresh();
    } else if (strncmp(argument1, "HLON\0", 5) == 0) {
      asynch_action_light_on();
    } 
    else
      return 6;
  }      
  else
  {
    // unknown command
    return 2;
  }
  return 0;
}


int safe_int_convert(char *string_data, long &variable)
{ /* Check that string_data can be converted to long before setting variable.
  
  Returns 1 if string data could not be converted to %d.
  */
  long conversion_var = 0;
  int status;
  
  // Parse into %d
  // Returns number of arguments successfully parsed
  status = sscanf(string_data, "%d", &conversion_var);
    
  //~ Serial.print("DBG SIC ");
  //~ Serial.print(string_data);
  //~ Serial.print("-");
  //~ Serial.print(conversion_var);
  //~ Serial.print("-");
  //~ Serial.print(status);
  //~ Serial.println(".");
  
  if (status == 1) {
    // Good, we converted one variable
    variable = conversion_var;
    return 0;
  }
  else {
    // Something went wrong, probably no variables converted
    Serial.print("ERR SIC cannot parse -");
    Serial.print(string_data);
    Serial.println("-");
    return 1;
  }
}


void asynch_action_reward_l()
{
  unsigned long time = millis();
  Serial.print(time);
  Serial.println(" EV AAR_L");
  digitalWrite(L_REWARD_VALVE, HIGH);
  delay(param_values[tpidx_REWARD_DUR_L]);
  digitalWrite(L_REWARD_VALVE, LOW); 
}

void asynch_action_reward_r()
{
  unsigned long time = millis();
  Serial.print(time);
  Serial.println(" EV AAR_R");
  digitalWrite(R_REWARD_VALVE, HIGH);
  delay(param_values[tpidx_REWARD_DUR_R]);
  digitalWrite(R_REWARD_VALVE, LOW); 
}

void asynch_action_reward()
{
  if (param_values[tpidx_REWSIDE] == LEFT)
    asynch_action_reward_l();
  else if (param_values[tpidx_REWSIDE] == RIGHT)
    asynch_action_reward_r();
  else
    Serial.println("ERR unknown rewside");
}

void asynch_action_set_thresh()
{
  unsigned long time = millis();
  Serial.print(time);
  Serial.println(" EV AAST");
  mpr121_setup(TOUCH_IRQ, param_values[tpidx_TOU_THRESH], 
    param_values[tpidx_REL_THRESH]);
}

void asynch_action_light_on()
{
  unsigned long time = millis();
  Serial.print(time);
  Serial.println(" EV HLON");
  digitalWrite(__HWCONSTANTS_H_HOUSE_LIGHT, HIGH);
}