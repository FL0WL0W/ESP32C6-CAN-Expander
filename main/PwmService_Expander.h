#include "Esp32IdfPwmService.h"
#include "PwmService_ATTiny427Expander.h"
#include "DigitalService_ATTiny427Expander.h"

#ifndef PWMSERVICE_EXPANDER_H
#define PWMSERVICE_EXPANDER_H
namespace EmbeddedIOServices
{
	class PwmService_Expander : public IPwmService
	{
	protected:
		Esp32::Esp32IdfPwmService *_esp32PwmService;
		PwmService_ATTiny427Expander *_attinyPwmService;
		DigitalService_ATTiny427Expander *_attinyDigitalService;
	public:
		PwmService_Expander(Esp32::Esp32IdfPwmService *esp32PwmService, PwmService_ATTiny427Expander *attinyPwmService, DigitalService_ATTiny427Expander *attinyDigitalService);
		void InitPin(pwmpin_t pin, PinDirection direction, uint16_t minFrequency);
		PwmValue ReadPin(pwmpin_t pin);
		void WritePin(pwmpin_t pin, PwmValue value);
	};
}
#endif
