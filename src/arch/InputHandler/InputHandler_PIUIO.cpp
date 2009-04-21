#include "global.h"
#include "RageLog.h"
#include "RageUtil.h"

#include "LightsManager.h"
#include "InputFilter.h"
#include "LuaManager.h"

/* for debug data */
#include "ScreenManager.h"
#include "Preference.h"

#include "DiagnosticsUtil.h"
#include "arch/Lights/LightsDriver_External.h"
#include "InputHandler_PIUIO.h"

// initialize the global usage flag
bool InputHandler_PIUIO::bInitialized = false;

static CString SensorNames[] = { "right", "left", "bottom", "top" };

static CString GetSensorDescription( uint32_t iArray[4], int iBit )
{
	CStringArray sensors;

	for( int i = 0; i < 4; i++ )
		if( iArray[i] & (1 << (31-iBit)) )
			sensors.push_back( SensorNames[i] );

	/* HACK: if all sensors are reporting, then don't return anything.
	 * On PIUIO, all buttons always return all sensors except pads. */
	if( sensors.size() == 4 )
		return "";

	return join(", ", sensors);
}

static CString DebugLine( const uint32_t iArray[4], const uint32_t &iWriteData )
{
	CString sRet = "Input:\n";

	for( int i = 0; i < 4; i++ )
	{
		sRet += "\t" + BitsToString( iArray[i] );
		sRet += "\n";
	}

	sRet += "Output:\n\t" + BitsToString( iWriteData );

	return sRet;
}

InputHandler_PIUIO::InputHandler_PIUIO()
{
	if( InputHandler_PIUIO::bInitialized )
	{
		LOG->Warn( "Redundant PIUIO driver loaded. Disabling..." );
		return;
	}

	m_bShutdown = false;

	// attempt to open and initialize the board
	m_bFoundDevice = Board.Open();

	if( m_bFoundDevice == false )
	{
		LOG->Warn( "Could not establish a connection with PIUIO." );
		return;
	}

	LOG->Trace( "Opened PIUIO board." );

	// set the relevant global flags (static flag, input type)
	InputHandler_PIUIO::bInitialized = true;
	DiagnosticsUtil::SetInputType( "PIUIO" );

	// set the handler's function pointer
	InternalInputHandler = &InputHandler_PIUIO::HandleInputNormal;

// use the r16 kernel hack code if it's available
#ifdef LINUX
	if( IsAFile("/rootfs/stats/patch/modules/usbcore.ko") )
		InternalInputHandler = &InputHandler_PIUIO::HandleInputKernel;
#endif

	SetLightsMappings();

	// tell us to report every 5000 updates,
	// but leave us to do the reporting.
	m_DebugTimer.m_sName = "MK6";
	m_DebugTimer.m_bAutoReport = false;
	m_DebugTimer.m_iReportInterval = 5;

	InputThread.SetName( "PIUIO thread" );
	InputThread.Create( InputThread_Start, this );
}

InputHandler_PIUIO::~InputHandler_PIUIO()
{
	// give a final report
	m_DebugTimer.Report();

	if( InputThread.IsCreated() )
	{
		m_bShutdown = true;
		LOG->Trace( "Shutting down PIUIO thread..." );
		InputThread.Wait();
		LOG->Trace( "PIUIO thread shut down." );
	}

	// reset all lights and unclaim the device
	if( m_bFoundDevice )
	{
		Board.Write( 0 );
		Board.Close();
		InputHandler_PIUIO::bInitialized = false;
	}
}

void InputHandler_PIUIO::GetDevicesAndDescriptions( vector<InputDevice>& vDevicesOut, vector<CString>& vDescriptionsOut )
{
	if( m_bFoundDevice )
	{
		vDevicesOut.push_back( InputDevice(DEVICE_JOY1) );
		vDescriptionsOut.push_back( "PIUIO" );
	}
}

void InputHandler_PIUIO::SetLightsMappings()
{
	uint32_t iCabinetLights[NUM_CABINET_LIGHTS] = 
	{
		/* UL, UR, LL, LR marquee lights */
		(1 << 23), (1 << 26), (1 << 25), (1 << 24),

		/* selection buttons (not used), bass lights */
		0, 0, (1 << 10), (1 << 10)
	};

	uint32_t iGameLights[MAX_GAME_CONTROLLERS][MAX_GAME_BUTTONS] = 
	{
		/* Left, Right, Up, Down */
		{ (1 << 20), (1 << 21), (1 << 18), (1 << 19) },	/* Player 1 */
		{ (1 << 4), (1 << 5), (1 << 2), (1 << 3) }	/* Player 2 */
	};

	m_LightsMappings.SetCabinetLights( iCabinetLights );
	m_LightsMappings.SetGameLights( iGameLights[GAME_CONTROLLER_1],
		iGameLights[GAME_CONTROLLER_2] );
	
	m_LightsMappings.m_iCoinCounterOn = (1 << 28);
	m_LightsMappings.m_iCoinCounterOff = (1 << 27);

	LightsMapper::LoadMappings( "PIUIO", m_LightsMappings );
}

void InputHandler_PIUIO::InputThreadMain()
{
	while( !m_bShutdown )
	{
		m_DebugTimer.StartUpdate();

		/* Figure out the lights and write them */
		UpdateLights();

		/* Find our sensors, report to RageInput */
		HandleInput();

		m_DebugTimer.EndUpdate();

		if( g_bDebugInputDrivers && m_DebugTimer.TimeToReport() )
		{
			m_DebugTimer.Report();
			CString sLine = DebugLine( m_iInputData, m_iLightData );
			SCREENMAN->SystemMessageNoAnimate( sLine );
		}
	}
}

/* WARNING: SCIENCE CONTENT!
 * We write each output set in members 0, 2, 4, and 6 of a uint32_t array.
 * The BulkReadWrite sends four asynchronous write/read requests that end
 * up overwriting the data we write with the data that's read.
 *
 * I'm not sure why we need an 8-member array. Oh well. */
void InputHandler_PIUIO::HandleInputKernel()
{
	ZERO( m_iBulkReadData );

	m_iLightData &= 0xFFFCFFFC;

	// write each light state at once - array members 0, 2, 4, and 6
	for (uint32_t i = 0; i < 4; i++)
		m_iBulkReadData[i*2] = m_iLightData | (i | (i << 16));

	Board.BulkReadWrite( m_iBulkReadData );

	// translate the sensor data to m_iInputData, and invert
	for (uint32_t i = 0; i < 4; i++)
		m_iInputData[i] = ~m_iBulkReadData[i*2];
}

/* this is the input-reading logic that we know works */
void InputHandler_PIUIO::HandleInputNormal()
{
	for (uint32_t i = 0; i < 4; i++)
	{
		// write which sensors to report from
		m_iLightData &= 0xFFFCFFFC;
		m_iLightData |= (i | (i << 16));

		// do one write/read cycle to get this set of sensors
		Board.Write( m_iLightData );
		Board.Read( &m_iInputData[i] );

		/* PIUIO opens high - for more logical processing, invert it */
		m_iInputData[i] = ~m_iInputData[i];
	}
}

void InputHandler_PIUIO::HandleInput()
{
	// reset our reading data
	ZERO( m_iInputField );
	ZERO( m_iInputData );

	// sets up m_iInputData for usage
	(this->*InternalInputHandler)();

	// combine the read data into a single field
	for( int i = 0; i < 4; i++ )
		m_iInputField |= m_iInputData[i];

	// construct outside the loop, to save some processor time
	DeviceInput di(DEVICE_JOY1, JOY_1);

	for( int iButton = 0; iButton < 32; iButton++ )
	{
		di.button = JOY_1+iButton;

		/* If we're in a thread, our timestamp is accurate */
		if( InputThread.IsCreated() )
			di.ts.Touch();

		/* Set a description of detected sensors to the arrows */
		INPUTFILTER->SetButtonComment( di, GetSensorDescription(m_iInputData, iButton) );

		/* Is the button we're looking for flagged in the input data? */
		ButtonPressed( di, m_iInputField & (1 << (31-iButton)) );
	}
}

/* Requires LightsDriver_External. */
void InputHandler_PIUIO::UpdateLights()
{
	// set a const pointer to the "ext" LightsState to read from
	static const LightsState *m_LightsState = LightsDriver_External::Get();

	// reset
	ZERO( m_iLightData );

	// update marquee lights
	FOREACH_CabinetLight( cl )
		if( m_LightsState->m_bCabinetLights[cl] )
			m_iLightData |= m_LightsMappings.m_iCabinetLights[cl];

	FOREACH_GameController( gc )
		FOREACH_GameButton( gb )
			if( m_LightsState->m_bGameButtonLights[gc][gb] )
				m_iLightData |= m_LightsMappings.m_iGameLights[gc][gb];

	/* The coin counter moves halfway if we send bit 4, then the
	 * rest of the way (or not at all) if we send bit 5. Send bit
	 * 5 unless we have a coin event being recorded. */
	m_iLightData |= m_LightsState->m_bCoinCounter ?
		m_LightsMappings.m_iCoinCounterOn : m_LightsMappings.m_iCoinCounterOff;
}

uint32_t InputHandler_PIUIO::GetSensorSet( int iSet )
{
	// bounds checking
	if( iSet >= 3 )
		return 0;

	return m_iInputData[iSet];
}

#include "LuaBinding.h"

template<class T>
class LunaInputHandler_PIUIO: public Luna<T>
{
public:
	LunaInputHandler_PIUIO() { LUA->Register( Register ); }

	static int GetSensorSet( T* p, lua_State *L )
	{
		vector<bool> vSensors;
		uint32_t iSensors = p->GetSensorSet( IArg(1) );

		for( int i = 0; i < 32; i++ )
		{
			bool temp = iSensors & (1 << (31-i));
			vSensors.push_back( temp );
		}

		LuaHelpers::CreateTableFromArrayB( L, vSensors );
		return 0;
	}

	static void Register( lua_State *L )
	{
		ADD_METHOD( GetSensorSet )
		Luna<T>::Register( L );
	}
};

// doesn't work yet...
//LUA_REGISTER_CLASS( InputHandler_PIUIO );

/*
 * (c) 2005 Chris Danford, Glenn Maynard.  Re-implemented by vyhd, infamouspat
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
