#include "PwmService_Expander.h"

using namespace Esp32;

#ifdef PWMSERVICE_EXPANDER_H
namespace EmbeddedIOServices
{
    PwmService_Expander::PwmService_Expander(Esp32IdfPwmService *esp32PwmService, PwmService_ATTiny427Expander *attinyPwmService, ATTiny427_PassthroughService *attinyPassthroughService, DigitalService_ATTiny427Expander *attinyDigitalService) :
		_esp32PwmService(esp32PwmService),
		_attinyPwmService(attinyPwmService),
		_attinyPassthroughService(attinyPassthroughService),
		_attinyDigitalService(attinyDigitalService)
    {
    }
	
	void PwmService_Expander::InitPin(pwmpin_t pin, PinDirection direction, uint16_t minFrequency)
	{
        switch(pin)
        {
			case 1:
				if(direction == Out)
				{
					_attinyDigitalService->InitPin(6, Out);
					_attinyDigitalService->WritePin(6, true);//disable CAN2
					_attinyPwmService->InitPin(9, Out, minFrequency);
				}
				else
				{
					_attinyPwmService->InitPin(19, In, minFrequency);
				}
				break;
			case 3:
				if(direction == Out)
				{
					_esp32PwmService->InitPin(4, Out, minFrequency);
				}
				else
				{
					_attinyPwmService->InitPin(8, In, minFrequency);
				}
				break;
			case 4:
				if(direction == Out)
				{
					_attinyPwmService->InitPin(10, Out, minFrequency);
				}
				else
				{
					_attinyPwmService->InitPin(13, In, minFrequency);
				}
				break;
			case 5:
				if(direction == Out)
				{
					_attinyPassthroughService->InitPassthrough(12,7,true);
					_esp32PwmService->InitPin(18, Out, minFrequency);
				}
				else
				{
					_attinyPassthroughService->InitPassthrough(7,12,false);
					_esp32PwmService->InitPin(18, In, minFrequency);
				}
				break;
			case 6:
				if(direction == Out)
				{
					_attinyPassthroughService->InitPassthrough(14,5,false, true);
					_esp32PwmService->InitPin(19, Out, minFrequency);
				}
				else
				{
					_attinyPassthroughService->InitPassthrough(5,14,false);
					_esp32PwmService->InitPin(19, In, minFrequency);
				}
				break;
			case 10:
				if(direction == Out)
				{
					_esp32PwmService->InitPin(3, Out, minFrequency);
					_attinyDigitalService->InitPin(6, Out);
					_attinyDigitalService->WritePin(6, true);//disable CAN2
				}
				break;
			case 13:
					_esp32PwmService->InitPin(17, direction, minFrequency);
				break;
			case 14:
					_esp32PwmService->InitPin(16, direction, minFrequency);
				break;
        }
	}
	PwmValue PwmService_Expander::ReadPin(pwmpin_t pin)
	{
		PwmValue value = PwmValue();
        switch(pin)
        {
			case 1:
				value = _attinyPwmService->ReadPin(19);
				break;
			case 3:
				value = _attinyPwmService->ReadPin(8);
				break;
			case 4:
				value = _attinyPwmService->ReadPin(13);
				break;
			case 5:
				value = _esp32PwmService->ReadPin(18);
				break;
			case 6:
				value = _esp32PwmService->ReadPin(19);
				break;
			case 7:
				value = _esp32PwmService->ReadPin(20);
				break;
			case 13:
				value = _esp32PwmService->ReadPin(17);
				break;
			case 14:
				value = _esp32PwmService->ReadPin(16);
				break;
			case 16:
				value = _esp32PwmService->ReadPin(21);
				break;
        }
		return value;
	}
	void PwmService_Expander::WritePin(pwmpin_t pin, PwmValue value)
	{
        switch(pin)
        {
			case 1:
				_attinyPwmService->WritePin(9, value);
				break;
			case 3:
				_esp32PwmService->WritePin(4, { value.Period, value.Period - value.PulseWidth });
				break;
			case 4:
				_attinyPwmService->WritePin(10, value);
				break;
			case 5:
				_esp32PwmService->WritePin(18, { value.Period, value.Period - value.PulseWidth });
				break;
			case 6:
				_esp32PwmService->WritePin(19, { value.Period, value.PulseWidth });
				break;
			case 7:
				_esp32PwmService->WritePin(20, { value.Period, value.Period - value.PulseWidth });
				break;
			case 10:
				_esp32PwmService->WritePin(3, { value.Period, value.Period - value.PulseWidth });
				break;
			case 13:
				_esp32PwmService->WritePin(17, value);
				break;
			case 14:
				_esp32PwmService->WritePin(16, value);
				break;
			case 16:
				_esp32PwmService->WritePin(21, { value.Period, value.Period - value.PulseWidth });
				break;
        }
	}
}
#endif
