#include "bb_primitives.h"
#include "bb_proc.h"
#include "bb_history.h"
#include "bb_register.h"

class BBHardwareIO {
  public:
  virtual bool ReadInput(uint8_t channel) = 0;
  virtual void WriteOutput(uint8_t channel, bool trigger, LightValue lv, uint8_t pwmLevel) = 0;
};

class BlinkBus {
  public:

  BlinkBus(BBHardwareIO* ioPtr) {
    m_io = ioPtr;
  }

  SwitchIOModel processorIO             {1, true};
  SwitchIOModel processorSensorOut      {1, false};
  RegisterModel<MasterRegister> master  {2};
  SwitchIOModel analogInputs            {3, true};
  SwitchIOModel analogOutputsReg        {4, true};

  RegisterModel<CommonRegister> debugger      {5};
  RegisterModel<CommonRegister> modbusSlaveId {8};
  RegisterModel<CommonRegister> modbusSpeed   {9};

  RegisterModel<CommonRegister> analogToProcMap[channel_count] = {
    RegisterModel<CommonRegister>(10),
    RegisterModel<CommonRegister>(11),
    RegisterModel<CommonRegister>(12),
    RegisterModel<CommonRegister>(13),
    RegisterModel<CommonRegister>(14),
    RegisterModel<CommonRegister>(15),
    RegisterModel<CommonRegister>(16),
    RegisterModel<CommonRegister>(17) 
  };

  RegisterModel<CommonRegister> lowPassMs{18};

  RegisterModel<CommonRegister> procToOutputMap[channel_count] = {
    RegisterModel<CommonRegister>(20),
    RegisterModel<CommonRegister>(21),
    RegisterModel<CommonRegister>(22),
    RegisterModel<CommonRegister>(23),
    RegisterModel<CommonRegister>(24),
    RegisterModel<CommonRegister>(25),
    RegisterModel<CommonRegister>(26),
    RegisterModel<CommonRegister>(27) 
  };

  RegisterModel<CommonRegister> intervalSmallMs{28};
  RegisterModel<CommonRegister> intervalBigMs{29};

  RegisterModel<CommonRegister> analogToGestureMap[channel_count] = {
    RegisterModel<CommonRegister>(30),
    RegisterModel<CommonRegister>(31),
    RegisterModel<CommonRegister>(32),
    RegisterModel<CommonRegister>(33),
    RegisterModel<CommonRegister>(34),
    RegisterModel<CommonRegister>(35),
    RegisterModel<CommonRegister>(36),
    RegisterModel<CommonRegister>(37) 
  };

  RegisterModel<GestureRegister> gestureToSceneMap[channel_count] = {
    RegisterModel<GestureRegister>(40),
    RegisterModel<GestureRegister>(41),
    RegisterModel<GestureRegister>(42),
    RegisterModel<GestureRegister>(43),
    RegisterModel<GestureRegister>(44),
    RegisterModel<GestureRegister>(45),
    RegisterModel<GestureRegister>(46),
    RegisterModel<GestureRegister>(47) 
  };

  RegisterModel<SceneActivateRegister> sceneActivation{48};
  //todo remote
  bool sceneActivationHandled = true;

  RegisterModel<CommonRegister> scenes[channel_count] = {
    RegisterModel<CommonRegister>(50),
    RegisterModel<CommonRegister>(51),
    RegisterModel<CommonRegister>(52),
    RegisterModel<CommonRegister>(53),
    RegisterModel<CommonRegister>(54),
    RegisterModel<CommonRegister>(55),
    RegisterModel<CommonRegister>(56),
    RegisterModel<CommonRegister>(57) 
  };

  RegisterModel<CommonRegister> pwmState[channel_count] = {
    RegisterModel<CommonRegister>(60),
    RegisterModel<CommonRegister>(61),
    RegisterModel<CommonRegister>(62),
    RegisterModel<CommonRegister>(63),
    RegisterModel<CommonRegister>(64),
    RegisterModel<CommonRegister>(65),
    RegisterModel<CommonRegister>(66),
    RegisterModel<CommonRegister>(67) 
  };

  //register 90-99 reserved for settings
  RegisterModel<CommonRegister> PwmMinLevel{90};
  RegisterModel<CommonRegister> PwmHalfLevel{91};

  void Process( int32_t currentTime ) {
    ReadAll();
    ProcessInternal(currentTime);
    WriteAll();
  }

  private:
  
  void ReadAll() {
    //read analog inputs
    for (int i = 0; i < channel_count; i++) {
      analogInputs.States[i].Set(m_io->ReadInput(i));
    } 

    //read remote registers
    processorIO.Mark();
    processorIO.Read();
  }

  void ProcessInternal( int32_t currentTime ) {
    //check history for gesture
    for (int i = 0; i < channel_count; i++) {

      //init channel processor
      if ( !m_channelProcessor[i].Inited ) {
        m_channelProcessor[i].Init(
          lowPassMs.Get().value, 
          master.Get().coils.GestureLag, 
          intervalSmallMs.Get().value, 
          intervalBigMs.Get().value);
      }

      m_channelProcessor[i].Step( analogInputs.States[i].Get() , currentTime );

      //no gestures map
      if (analogToGestureMap[i].Get().words.first == 0) {
        continue;
      }

      Gesture g = m_channelProcessor[i].GestureValidate(currentTime);
      if (g != Gesture::Nope) {
        debugger.SetFirstWord((int)g);
        //map input to gestures
        forEach8Bit(gestureChannel, analogToGestureMap[i].Get().words.first) {
          GestureRegister gesture = gestureToSceneMap[gestureChannel.Get()].Get();
          if (gesture.coils.type == (int)g) {
            //first finded gesture set to activate
            SceneActivateRegister sa;
            sa.value = gesture.value;
            sa.coils.rttCh = gestureChannel.Get();
            sceneActivation.Set(sa);
            sceneActivationHandled = false;
            break;
          }
        }
      }
    }

    //apply gesture to scene
    if (!sceneActivationHandled) {
      sceneActivationHandled = true;
      SceneActivateRegister sa = sceneActivation.Get();

      Action currentAction = static_cast<Action>(sa.coils.action);

      //handling scene rotation
      Int8RegIterator gtam(sa.coils.map);
      if (sa.coils.rotate && m_gestureRotation[sa.coils.rttCh] > gtam.GetCount() - 1) {
        m_gestureRotation[sa.coils.rttCh] = 0;
      }

      int sceneIndex = 0;
      for (gtam.Reset(); gtam.HasNext(); gtam.Step(), sceneIndex++) {
        
        //skip inactive scenes 
        if (sa.coils.rotate && sceneIndex != m_gestureRotation[sa.coils.rttCh] ) {
          continue;
        }

        int sceneChannel = gtam.Get();
        RegisterModel<CommonRegister> sceneMap = scenes[sceneChannel];

        //scene map itetate affected channel
        forEach8Bit(affectedChannel, sceneMap.Get().words.second) {
          bool sceneMapValue = sceneMap.GetWordBit(true, affectedChannel.Get());

          if (sa.coils.procOrOut) {
            //set action to proccessor
            m_zoneProcessors[affectedChannel.Get()].SignalGesture.Set(sceneMapValue ? currentAction : Action::Off);
          } else {
            //apply action to outputs directly
            LightValue currentValue = m_analogOutputs[affectedChannel.Get()].Get();
            m_analogOutputs[affectedChannel.Get()].Set(sceneMapValue ? ApplyActionToCurrentValue(currentValue, currentAction ) : LightValue::Off);
          }
        }
      }
    }

    //set master states to proc
    for (int i = 0; i < channel_count; i++) {
      m_zoneProcessors[i].SignalMaster.Set(master.Get().coils.MasterSwitch);
      m_zoneProcessors[i].StateSensorDayMode = master.Get().coils.DayMode;
      m_zoneProcessors[i].StateSensorEveningMode = master.Get().coils.EveningMode;
    }

    //map analog input to processors
    for (int i = 0; i < channel_count; i++) {
      if (!m_channelProcessor[i].FilteredState.HasChanges()) {
        continue;
      }
      //iterate all switch channels
      
      forEach8Bit(procNum, analogToProcMap[i].Get().words.first) {
        m_zoneProcessors[procNum.Get()].SignalSwitch.Set(m_channelProcessor[i].FilteredState.Get());
      }
      //iterate all sensor channels
      forEach8Bit(procNumS, analogToProcMap[i].Get().words.second) {
        m_zoneProcessors[procNumS.Get()].SignalSensor.Set(m_channelProcessor[i].FilteredState.Get());
      }

      m_channelProcessor[i].FilteredState.MarkHandled();
    } 

    //set remote to processors
    for (int i = 0; i < channel_count; i++) {
      if (!processorIO.States[i].HasChanges()) {
        continue;
      }
      m_zoneProcessors[i].SignalSwitch.Set(processorIO.States[i].Get());
      processorIO.States[i].MarkHandled();
    } 

    //run processor and apply results
    for (int i = 0; i < channel_count; i++) {
      ZoneProcessor*processor = &m_zoneProcessors[i];
      
      processor->Step();
      processorIO.States[i].Set(processor->OutputState.Get() != LightValue::Off);

      processorSensorOut.States[i].Set(
        processor->OutputState.Get() == LightValue::Half || 
        processor->OutputState.Get() == LightValue::Min
        );

      if (!processor->OutputState.HasChanges()) {
        continue;
      }

      //iterate all enabled channels
      forEach8Bit(analogOutputChannel, procToOutputMap[i].Get().words.first) {
        m_analogOutputs[analogOutputChannel.Get()].Set(processor->OutputState.Get());
      }

      processor->OutputState.MarkHandled();
    }
  }

  void WriteAll() {
    for (int i = 0; i < channel_count; i++) {
      LightValue value = m_analogOutputs[i].Get();
      if (m_analogOutputs[i].HasChanges()) {
        int pwm = 255;
        if (value == LightValue::Off) {
          pwm = 0;
        } else if (value == LightValue::Half) {
          pwm = PwmHalfLevel.Get().value;
        } else if (value == LightValue::Min) {
          pwm = PwmMinLevel.Get().value;
        }
        pwmState[i].Set(pwm);
        analogOutputsReg.States[i].Set(value != LightValue::Off);
      }

      m_io->WriteOutput(i, m_analogOutputs[i].HasChanges(), value, pwmState[i].Get().value);

      m_analogOutputs[i].MarkHandled();
    } 

    processorIO.Write();
    analogInputs.Write();
    processorSensorOut.Write();
    analogOutputsReg.Write();
  }

  BBHardwareIO* m_io;
  State<LightValue> m_analogOutputs[channel_count];
  ZoneProcessor m_zoneProcessors[channel_count];
  InputChannelProcessor m_channelProcessor[channel_count];
  int m_gestureRotation[channel_count];
};