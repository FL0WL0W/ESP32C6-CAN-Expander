#include "AnalogService_Expander.h"

using namespace Esp32;

#ifdef ANALOGSERVICE_EXPANDER_H
namespace EmbeddedIOServices
{
    AnalogService_Expander::AnalogService_Expander(Esp32IdfAnalogService *esp32AnalogService, AnalogService_ATTiny427Expander *attinyAnalogService) :
		_esp32AnalogService(esp32AnalogService),
		_attinyAnalogService(attinyAnalogService)
    {
    }
	
	void AnalogService_Expander::InitPin(analogpin_t pin)
	{
        switch(pin)
        {
            case 1:
				_attinyAnalogService->InitPin(19);
				break;
            case 3:
				_attinyAnalogService->InitPin(8);
				break;
            case 4:
				_attinyAnalogService->InitPin(13);
				break;
            case 5:
				_attinyAnalogService->InitPin(7);
				break;
            case 6:
				_attinyAnalogService->InitPin(5);
				break;
            case 7:
				_attinyAnalogService->InitPin(18);
				break;
            case 16:
				_attinyAnalogService->InitPin(17);
				break;
		}
	}

	float AnalogService_Expander::ReadPin(analogpin_t pin)
	{
        switch(pin)
        {
            case 1:
				return _attinyAnalogService->ReadPin(19);
            case 3:
				return _attinyAnalogService->ReadPin(8);
            case 4:
				return _attinyAnalogService->ReadPin(13);
            case 5:
				return _attinyAnalogService->ReadPin(7);
            case 6:
				return _attinyAnalogService->ReadPin(5);
            case 7:
				return _attinyAnalogService->ReadPin(18);
            case 16:
				return _attinyAnalogService->ReadPin(17);
		}
		return 0;
	}
}
#endif
