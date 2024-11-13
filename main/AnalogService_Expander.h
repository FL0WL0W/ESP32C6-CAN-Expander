#include "Esp32IdfAnalogService.h"
#include "AnalogService_ATTiny427Expander.h"
#ifndef ANALOGSERVICE_EXPANDER_H
#define ANALOGSERVICE_EXPANDER_H
namespace EmbeddedIOServices
{
	class AnalogService_Expander : public IAnalogService
	{
	protected:
		Esp32::Esp32IdfAnalogService *_esp32AnalogService;
		AnalogService_ATTiny427Expander *_attinyAnalogService;
	public:
		AnalogService_Expander(Esp32::Esp32IdfAnalogService *esp32AnalogService, AnalogService_ATTiny427Expander *attinyAnalogService);
		void InitPin(analogpin_t pin);
		float ReadPin(analogpin_t pin);
	};
}
#endif
