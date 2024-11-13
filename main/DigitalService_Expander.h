#include "Esp32IdfDigitalService.h"
#include "DigitalService_ATTiny427Expander.h"

#ifndef DIGITALSERVICE_EXPANDER_H
#define DIGITALSERVICE_EXPANDER_H
namespace EmbeddedIOServices
{
	class DigitalService_Expander : public IDigitalService
	{
	protected:
		Esp32::Esp32IdfDigitalService *_esp32DigitalService;
		DigitalService_ATTiny427Expander *_attinyDigitalService;
		bool In5 : 1;
		bool In6 : 1;
		bool In7 : 1;
		bool In16 : 1;
	public:
		DigitalService_Expander(Esp32::Esp32IdfDigitalService *esp32DigitalService, DigitalService_ATTiny427Expander *attinyDigitalService);
		void InitPin(digitalpin_t pin, PinDirection direction);
		bool ReadPin(digitalpin_t pin);
		void WritePin(digitalpin_t pin, bool value);
		void AttachInterrupt(digitalpin_t pin, callback_t callBack);
		void DetachInterrupt(digitalpin_t pin);
	};
}
#endif
