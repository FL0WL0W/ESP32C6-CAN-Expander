#include "DigitalService_Expander.h"

using namespace Esp32;

#ifdef DIGITALSERVICE_EXPANDER_H
namespace EmbeddedIOServices
{
    DigitalService_Expander::DigitalService_Expander(Esp32IdfDigitalService *esp32DigitalService, DigitalService_ATTiny427Expander *attinyDigitalService, ATTiny427_PassthroughService *attinyPassthroughService) :
		_esp32DigitalService(esp32DigitalService),
		_attinyDigitalService(attinyDigitalService),
		_attinyPassthroughService(attinyPassthroughService)
    {
    }
	
	void DigitalService_Expander::InitPin(digitalpin_t pin, PinDirection direction)
	{
        switch(pin)
        {
            case 1:
				_attinyDigitalService->WritePin(6, 1);
				_attinyDigitalService->InitPin(6, Out);
				_attinyDigitalService->InitPin(9, direction);
				_attinyDigitalService->InitPin(19, In);
				break;
            case 3:
				_esp32DigitalService->InitPin(4, direction);
				_attinyDigitalService->InitPin(8, In);
				break;
            case 4:
				_attinyDigitalService->InitPin(10, direction);
				_attinyDigitalService->InitPin(13, In);
				break;
            case 5:
				_esp32DigitalService->InitPin(18, direction);
				if(In5 == (direction == In))
					_attinyPassthroughService->InitPassthrough(7, 12, false);
				else
					_attinyPassthroughService->InitPassthrough(12, 7, true);
				break;
            case 6:
				_esp32DigitalService->InitPin(19, direction);
				if(In6 == (direction == In))
					_attinyPassthroughService->InitPassthrough(5, 14, false);
				else
					_attinyPassthroughService->InitPassthrough(14, 5, true);
				break;
            case 7:
				_esp32DigitalService->InitPin(20, direction);
				if(In7 == (direction == In))
					_attinyPassthroughService->InitPassthrough(18, 15, false);
				else
					_attinyPassthroughService->InitPassthrough(15, 18, true);
				break;
            case 10:
				if(direction == Out)
				{
					_esp32DigitalService->WritePin(3, 1);
					_esp32DigitalService->InitPin(3, Out);
					_attinyDigitalService->WritePin(6, 1);
					_attinyDigitalService->InitPin(6, Out);
				}
				break;
			case 12:
				_esp32DigitalService->InitPin(9, direction);
				break;
			case 13:
				_esp32DigitalService->InitPin(17, direction);
				break;
			case 14:
				_esp32DigitalService->InitPin(16, direction);
				break;
			case 15:
				_esp32DigitalService->InitPin(5, direction);
				break;
            case 16:
				_esp32DigitalService->InitPin(21, direction);
				if(In16 == (direction == In))
					_attinyPassthroughService->InitPassthrough(17, 20., false);
				else
					_attinyPassthroughService->InitPassthrough(20, 17, true);
				break;
        }
	}
	bool DigitalService_Expander::ReadPin(digitalpin_t pin)
	{
        switch(pin)
        {
            case 1:
				return _attinyDigitalService->ReadPin(19);
            case 3:
				return _attinyDigitalService->ReadPin(8);
            case 4:
				return _attinyDigitalService->ReadPin(13);
            case 5:
				if(In5)
					return _esp32DigitalService->ReadPin(18);
				return _attinyDigitalService->ReadPin(7);
            case 6:
				if(In6)
					return _esp32DigitalService->ReadPin(19);
				return _attinyDigitalService->ReadPin(5);
            case 7:
				if(In7)
					return _esp32DigitalService->ReadPin(20);
				return _attinyDigitalService->ReadPin(18);
            case 12:
				return _esp32DigitalService->ReadPin(9);
            case 13:
				return _esp32DigitalService->ReadPin(17);
            case 14:
				return _esp32DigitalService->ReadPin(16);
            case 15:
				return _esp32DigitalService->ReadPin(5);
            case 16:
				if(In16)
					return _esp32DigitalService->ReadPin(21);
				return _attinyDigitalService->ReadPin(17);
        }
		return false;
	}
	void DigitalService_Expander::WritePin(digitalpin_t pin, bool value)
	{
        switch(pin)
        {
            case 1:
				return _attinyDigitalService->WritePin(9, !value);
            case 3:
				return _esp32DigitalService->WritePin(4, value);
            case 4:
				return _attinyDigitalService->WritePin(10, value);
            case 5:
				return _esp32DigitalService->WritePin(18, !value);
            case 6:
				return _esp32DigitalService->WritePin(19, !value);
            case 7:
				return _esp32DigitalService->WritePin(20, !value);
			case 10:
				return _esp32DigitalService->WritePin(3, value);
			case 12:
				return _esp32DigitalService->WritePin(9, value);
			case 13:
				return _esp32DigitalService->WritePin(17, value);
			case 14:
				return _esp32DigitalService->WritePin(16, value);
			case 15:
				return _esp32DigitalService->WritePin(5, value);
            case 16:
				return _esp32DigitalService->WritePin(21, !value);
        }
	}
	void DigitalService_Expander::AttachInterrupt(digitalpin_t pin, callback_t callBack)
	{
        switch(pin)
        {
            case 1:
				return _attinyDigitalService->AttachInterrupt(19, callBack);
            case 3:
				return _attinyDigitalService->AttachInterrupt(8, callBack);
            case 4:
				return _attinyDigitalService->AttachInterrupt(13, callBack);
            case 5:
				if(In5)
					return _esp32DigitalService->AttachInterrupt(18, callBack);
				return _attinyDigitalService->AttachInterrupt(7, callBack);
            case 6:
				if(In6)
					return _esp32DigitalService->AttachInterrupt(19, callBack);
				return _attinyDigitalService->AttachInterrupt(5, callBack);
            case 7:
				if(In7)
					return _esp32DigitalService->AttachInterrupt(20, callBack);
				return _attinyDigitalService->AttachInterrupt(18, callBack);
            case 12:
				return _esp32DigitalService->AttachInterrupt(9, callBack);
            case 13:
				return _esp32DigitalService->AttachInterrupt(17, callBack);
            case 14:
				return _esp32DigitalService->AttachInterrupt(16, callBack);
            case 15:
				return _esp32DigitalService->AttachInterrupt(5, callBack);
            case 16:
				if(In16)
					return _esp32DigitalService->AttachInterrupt(21, callBack);
				return _attinyDigitalService->AttachInterrupt(17, callBack);
        }
	}
	void DigitalService_Expander::DetachInterrupt(digitalpin_t pin)
	{
        switch(pin)
        {
            case 1:
				return _attinyDigitalService->DetachInterrupt(19);
            case 3:
				return _attinyDigitalService->DetachInterrupt(8);
            case 4:
				return _attinyDigitalService->DetachInterrupt(13);
            case 5:
				_esp32DigitalService->DetachInterrupt(18);
				return _attinyDigitalService->DetachInterrupt(7);
            case 6:
				_esp32DigitalService->DetachInterrupt(19);
				return _attinyDigitalService->DetachInterrupt(5);
            case 7:
				_esp32DigitalService->DetachInterrupt(20);
				return _attinyDigitalService->DetachInterrupt(18);
            case 12:
				return _esp32DigitalService->DetachInterrupt(9);
            case 13:
				return _esp32DigitalService->DetachInterrupt(17);
            case 14:
				return _esp32DigitalService->DetachInterrupt(16);
            case 15:
				return _esp32DigitalService->DetachInterrupt(5);
            case 16:
				_esp32DigitalService->DetachInterrupt(21);
				return _attinyDigitalService->DetachInterrupt(17);
        }
	}
}
#endif
