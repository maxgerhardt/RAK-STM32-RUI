/*!
 * \file      RegionLA915.c
 *
 * \brief     Region implementation for LA915
 *
 * \copyright Revised BSD License, see section \ref LICENSE.
 *
 * \code
 *                ______                              _
 *               / _____)             _              | |
 *              ( (____  _____ ____ _| |_ _____  ____| |__
 *               \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 *               _____) ) ____| | | || |_| ____( (___| | | |
 *              (______/|_____)_|_|_| \__)_____)\____)_| |_|
 *              (C)2013-2017 Semtech
 *
 *               ___ _____ _   ___ _  _____ ___  ___  ___ ___
 *              / __|_   _/_\ / __| |/ / __/ _ \| _ \/ __| __|
 *              \__ \ | |/ _ \ (__| ' <| _| (_) |   / (__| _|
 *              |___/ |_/_/ \_\___|_|\_\_| \___/|_|_\\___|___|
 *              embedded.connectivity.solutions===============
 *
 * \endcode
 *
 * \author    Miguel Luis ( Semtech )
 *
 * \author    Gregory Cristian ( Semtech )
 *
 * \author    Daniel Jaeckle ( STACKFORCE )
*/
#ifdef REGION_LA915
#include "utilities.h"
#include "radio.h"
#include "RegionCommon.h"
#include "RegionLA915.h"

// Definitions
#define CHANNELS_MASK_SIZE              6

// A mask to select only valid 500KHz channels
#define CHANNELS_MASK_500KHZ_MASK       0x00FF

/*!
 * Region specific context
 */
typedef struct sRegionLA915NvmCtx
{
    /*!
     * LoRaMAC channels
     */
    ChannelParams_t Channels[ LA915_MAX_NB_CHANNELS ];
    /*!
     * LoRaMac bands
     */
    Band_t Bands[ LA915_MAX_NB_BANDS ];
    /*!
     * LoRaMac channels mask
     */
    uint16_t ChannelsMask[ CHANNELS_MASK_SIZE ];
    /*!
     * LoRaMac channels remaining
     */
    uint16_t ChannelsMaskRemaining[CHANNELS_MASK_SIZE];
    /*!
     * LoRaMac channels default mask
     */
    uint16_t ChannelsDefaultMask[ CHANNELS_MASK_SIZE ];
}RegionLA915NvmCtx_t;

/*
 * Non-volatile module context.
 */
static RegionLA915NvmCtx_t NvmCtx;

// Static functions
static int8_t GetNextLowerTxDr( int8_t dr, int8_t minDr )
{
    uint8_t nextLowerDr = 0;

    if( dr == minDr )
    {
        nextLowerDr = minDr;
    }
    else if( dr == DR_8 )
    {   // DR_7 is not allowed
        nextLowerDr = DR_6;
    }
    else
    {
        nextLowerDr = dr - 1;
    }
    return nextLowerDr;
}

static uint32_t GetBandwidth( uint32_t drIndex )
{
    switch( BandwidthsLA915[drIndex] )
    {
        default:
        case 125000:
            return 0;
        case 250000:
            return 1;
        case 500000:
            return 2;
    }
}

static int8_t LimitTxPower( int8_t txPower, int8_t maxBandTxPower, int8_t datarate, uint16_t* channelsMask )
{
    int8_t txPowerResult = txPower;

    // Limit tx power to the band max
    txPowerResult =  MAX( txPower, maxBandTxPower );

    return txPowerResult;
}

static bool VerifyRfFreq( uint32_t freq )
{
    // Check radio driver support
    if( Radio.CheckRfFrequency( freq ) == false )
    {
        return false;
    }

    // Rx frequencies
    if( ( freq < LA915_FIRST_RX1_CHANNEL ) ||
        ( freq > LA915_LAST_RX1_CHANNEL ) ||
        ( ( ( freq - ( uint32_t ) LA915_FIRST_RX1_CHANNEL ) % ( uint32_t ) LA915_STEPWIDTH_RX1_CHANNEL ) != 0 ) )
    {
        return false;
    }

    // Tx frequencies for 125kHz
    // Also includes the range for 500kHz channels
    if( ( freq < 915200000 ) ||  ( freq > 927800000 ) )
    {
        return false;
    }
    return true;
}

static TimerTime_t GetTimeOnAir( int8_t datarate, uint16_t pktLen )
{
    int8_t phyDr = DataratesLA915[datarate];
    uint32_t bandwidth = RegionCommonGetBandwidth( datarate, BandwidthsLA915 );

    return Radio.TimeOnAir( MODEM_LORA, bandwidth, phyDr, 1, 8, false, pktLen, true );
}


static uint8_t CountNbOfEnabledChannels( uint8_t datarate, uint16_t* channelsMask, ChannelParams_t* channels, Band_t* bands, uint8_t* enabledChannels, uint8_t* delayTx )
{
    uint8_t nbEnabledChannels = 0;
    uint8_t delayTransmission = 0;

    for( uint8_t i = 0, k = 0; i < LA915_MAX_NB_CHANNELS; i += 16, k++ )
    {
        for( uint8_t j = 0; j < 16; j++ )
        {
            if( ( channelsMask[k] & ( 1 << j ) ) != 0 )
            {
                if( channels[i + j].Frequency == 0 )
                { // Check if the channel is enabled
                    continue;
                }
                if( RegionCommonValueInRange( datarate, channels[i + j].DrRange.Fields.Min,
                                              channels[i + j].DrRange.Fields.Max ) == false )
                { // Check if the current channel selection supports the given datarate
                    continue;
                }
                if( bands[channels[i + j].Band].ReadyForTransmission == false )
                { // Check if the band is available for transmission
                    delayTransmission++;
                    continue;
                }
                enabledChannels[nbEnabledChannels++] = i + j;
            }
        }
    }

    *delayTx = delayTransmission;
    return nbEnabledChannels;
}

PhyParam_t RegionLA915GetPhyParam( GetPhyParams_t* getPhy )
{
    PhyParam_t phyParam = { 0 };

    switch( getPhy->Attribute )
    {
        case PHY_MIN_RX_DR:
        {
            if( getPhy->DownlinkDwellTime == 0)
            {
                phyParam.Value = LA915_RX_MIN_DATARATE;
            }
            else
            {
                phyParam.Value = LA915_DWELL_LIMIT_DATARATE;
            }
            break;
        }
        case PHY_MIN_TX_DR:
        {
            if( getPhy->UplinkDwellTime == 0)
            {
                phyParam.Value = LA915_TX_MIN_DATARATE;
            }
            else
            {
                phyParam.Value = LA915_DWELL_LIMIT_DATARATE;
            }
            break;
        }
        case PHY_DEF_TX_DR:
        {
            phyParam.Value = LA915_DEFAULT_DATARATE;
            break;
        }
        case PHY_NEXT_LOWER_TX_DR:
        {
            if( getPhy->UplinkDwellTime == 0)
            {
                phyParam.Value = GetNextLowerTxDr( getPhy->Datarate, LA915_TX_MIN_DATARATE );
            }
            else
            {
                phyParam.Value = GetNextLowerTxDr( getPhy->Datarate, LA915_DWELL_LIMIT_DATARATE );
            }
            break;
        }
        case PHY_MAX_TX_POWER:
        {
            phyParam.Value = LA915_MAX_TX_POWER;
            break;
        }
        case PHY_DEF_TX_POWER:
        {
            phyParam.Value = LA915_DEFAULT_TX_POWER;
            break;
        }
        case PHY_DEF_ADR_ACK_LIMIT:
        {
            phyParam.Value = LA915_ADR_ACK_LIMIT;
            break;
        }
        case PHY_DEF_ADR_ACK_DELAY:
        {
            phyParam.Value = LA915_ADR_ACK_DELAY;
            break;
        }
        case PHY_MAX_PAYLOAD:
        {
            if( getPhy->UplinkDwellTime == 0 )
            {
                phyParam.Value = MaxPayloadOfDatarateDwell0LA915[getPhy->Datarate];
            }
            else
            {
                phyParam.Value = MaxPayloadOfDatarateDwell1LA915[getPhy->Datarate];
            }
            break;
        }
        /*case PHY_MAX_PAYLOAD_REPEATER:
        {
            if( getPhy->UplinkDwellTime == 0)
            {
                phyParam.Value = MaxPayloadOfDatarateRepeaterDwell0LA915[getPhy->Datarate];
            }
            else
            {
                phyParam.Value = MaxPayloadOfDatarateRepeaterDwell1LA915[getPhy->Datarate];
            }
            break;
        }*/
        case PHY_DUTY_CYCLE:
        {
            phyParam.Value = LA915_DUTY_CYCLE_ENABLED;
            break;
        }
        case PHY_MAX_RX_WINDOW:
        {
            phyParam.Value = LA915_MAX_RX_WINDOW;
            break;
        }
        case PHY_RECEIVE_DELAY1:
        {
            phyParam.Value = LA915_RECEIVE_DELAY1;
            break;
        }
        case PHY_RECEIVE_DELAY2:
        {
            phyParam.Value = LA915_RECEIVE_DELAY2;
            break;
        }
        case PHY_JOIN_ACCEPT_DELAY1:
        {
            phyParam.Value = LA915_JOIN_ACCEPT_DELAY1;
            break;
        }
        case PHY_JOIN_ACCEPT_DELAY2:
        {
            phyParam.Value = LA915_JOIN_ACCEPT_DELAY2;
            break;
        }
        case PHY_RETRANSMIT_TIMEOUT:
        {
            phyParam.Value = ( LA915_ACKTIMEOUT + randr( -LA915_ACK_TIMEOUT_RND, LA915_ACK_TIMEOUT_RND ) );
            break;
        }
        case PHY_DEF_DR1_OFFSET:
        {
            phyParam.Value = LA915_DEFAULT_RX1_DR_OFFSET;
            break;
        }
        case PHY_DEF_RX2_FREQUENCY:
        {
            phyParam.Value = LA915_RX_WND_2_FREQ;
            break;
        }
        case PHY_DEF_RX2_DR:
        {
            phyParam.Value = LA915_RX_WND_2_DR;
            break;
        }
        case PHY_CHANNELS_MASK:
        {
            phyParam.ChannelsMask = NvmCtx.ChannelsMask;
            break;
        }
        case PHY_CHANNELS_DEFAULT_MASK:
        {
            phyParam.ChannelsMask = NvmCtx.ChannelsDefaultMask;
            break;
        }
        case PHY_MAX_NB_CHANNELS:
        {
            phyParam.Value = LA915_MAX_NB_CHANNELS;
            break;
        }
        case PHY_CHANNELS:
        {
            phyParam.Channels = NvmCtx.Channels;
            break;
        }
        case PHY_DEF_UPLINK_DWELL_TIME:
        {
            phyParam.Value = LA915_DEFAULT_UPLINK_DWELL_TIME;
            break;
        }
        case PHY_DEF_DOWNLINK_DWELL_TIME:
        {
            phyParam.Value = LA915_DEFAULT_DOWNLINK_DWELL_TIME;
            break;
        }
        case PHY_DEF_MAX_EIRP:
        {
            phyParam.fValue = LA915_DEFAULT_MAX_EIRP;
            break;
        }
        case PHY_DEF_ANTENNA_GAIN:
        {
            phyParam.fValue = LA915_DEFAULT_ANTENNA_GAIN;
            break;
        }
        case PHY_BEACON_FORMAT:
        {
            phyParam.BeaconFormat.BeaconSize = LA915_BEACON_SIZE;
            phyParam.BeaconFormat.Rfu1Size = LA915_RFU1_SIZE;
            phyParam.BeaconFormat.Rfu2Size = LA915_RFU2_SIZE;
            break;
        }
        case PHY_BEACON_CHANNEL_DR:
        {
            phyParam.Value = LA915_BEACON_CHANNEL_DR;
            break;
        }
        /*case PHY_BEACON_CHANNEL_STEPWIDTH:
        {
            phyParam.Value = LA915_BEACON_CHANNEL_STEPWIDTH;
            break;
        }*/
        case PHY_BEACON_NB_CHANNELS:
        {
            phyParam.Value = LA915_BEACON_NB_CHANNELS;
            break;
        }
        case PHY_PING_SLOT_CHANNEL_DR:
        {
            phyParam.Value = LA915_PING_SLOT_CHANNEL_DR;
            break;
        }
        default:
        {
            break;
        }
    }

    return phyParam;
}

void RegionLA915SetBandTxDone( SetBandTxDoneParams_t* txDone )
{
    RegionCommonSetBandTxDone( &NvmCtx.Bands[NvmCtx.Channels[txDone->Channel].Band], txDone->LastTxAirTime, txDone->Joined , txDone->ElapsedTimeSinceStartUp);
}

void RegionLA915InitDefaults( InitDefaultsParams_t* params )
{
    Band_t bands[LA915_MAX_NB_BANDS] =
    {
        LA915_BAND0
    };

    switch( params->Type )
    {
        case INIT_TYPE_DEFAULTS:
        {
            // Initialize bands
            memcpy1( ( uint8_t* )NvmCtx.Bands, ( uint8_t* )bands, sizeof( Band_t ) * LA915_MAX_NB_BANDS );

            // Channels
            // 125 kHz channels
            for( uint8_t i = 0; i < LA915_MAX_NB_CHANNELS - 8; i++ )
            {
                NvmCtx.Channels[i].Frequency = 915200000 + i * 200000;
                NvmCtx.Channels[i].DrRange.Value = ( DR_5 << 4 ) | DR_0;
                NvmCtx.Channels[i].Band = 0;
            }
            // 500 kHz channels
            for( uint8_t i = LA915_MAX_NB_CHANNELS - 8; i < LA915_MAX_NB_CHANNELS; i++ )
            {
                NvmCtx.Channels[i].Frequency = 915900000 + ( i - ( LA915_MAX_NB_CHANNELS - 8 ) ) * 1600000;
                NvmCtx.Channels[i].DrRange.Value = ( DR_6 << 4 ) | DR_6;
                NvmCtx.Channels[i].Band = 0;
            }

            // Initialize channels default mask
            NvmCtx.ChannelsDefaultMask[0] = 0xFF;
            NvmCtx.ChannelsDefaultMask[1] = 0;
            NvmCtx.ChannelsDefaultMask[2] = 0;
            NvmCtx.ChannelsDefaultMask[3] = 0;
            NvmCtx.ChannelsDefaultMask[4] = 0;
            NvmCtx.ChannelsDefaultMask[5] = 0;

            // Copy channels default mask
            RegionCommonChanMaskCopy( NvmCtx.ChannelsMask, NvmCtx.ChannelsDefaultMask, 6 );

            // Copy into channels mask remaining
            RegionCommonChanMaskCopy( NvmCtx.ChannelsMaskRemaining, NvmCtx.ChannelsMask, 6 );
            break;
        }
        case INIT_TYPE_RESET_TO_DEFAULT_CHANNELS:
        {
            /*if( params->NvmCtx != 0 )
            {
                memcpy1( (uint8_t*) &NvmCtx, (uint8_t*) params->NvmCtx, sizeof( NvmCtx ) );
            }
            break;*/
        }
        case INIT_TYPE_ACTIVATE_DEFAULT_CHANNELS:
        {
            // Copy channels default mask
            RegionCommonChanMaskCopy( NvmCtx.ChannelsMask, NvmCtx.ChannelsDefaultMask, 6 );

            for( uint8_t i = 0; i < 6; i++ )
            { // Copy-And the channels mask
                NvmCtx.ChannelsMaskRemaining[i] &= NvmCtx.ChannelsMask[i];
            }
            break;
        }
        default:
        {
            break;
        }
    }
}

/*void* RegionLA915GetNvmCtx( GetNvmCtxParams_t* params )
{
    params->nvmCtxSize = sizeof( RegionLA915NvmCtx_t );
    return &NvmCtx;
}*/

bool RegionLA915Verify( VerifyParams_t* verify, PhyAttribute_t phyAttribute )
{
    switch( phyAttribute )
    {
        case PHY_FREQUENCY:
        {
            return VerifyRfFreq( verify->Frequency );
        }
        case PHY_TX_DR:
        case PHY_DEF_TX_DR:
        {
            if( verify->DatarateParams.UplinkDwellTime == 0 )
            {
                return RegionCommonValueInRange( verify->DatarateParams.Datarate, LA915_TX_MIN_DATARATE, LA915_TX_MAX_DATARATE );
            }
            else
            {
                return RegionCommonValueInRange( verify->DatarateParams.Datarate, LA915_DWELL_LIMIT_DATARATE, LA915_TX_MAX_DATARATE );
            }
        }
        case PHY_RX_DR:
        {
            if( verify->DatarateParams.UplinkDwellTime == 0 )
            {
                return RegionCommonValueInRange( verify->DatarateParams.Datarate, LA915_RX_MIN_DATARATE, LA915_RX_MAX_DATARATE );
            }
            else
            {
                return RegionCommonValueInRange( verify->DatarateParams.Datarate, LA915_DWELL_LIMIT_DATARATE, LA915_RX_MAX_DATARATE );
            }
        }
        case PHY_DEF_TX_POWER:
        case PHY_TX_POWER:
        {
            // Remark: switched min and max!
            return RegionCommonValueInRange( verify->TxPower, LA915_MAX_TX_POWER, LA915_MIN_TX_POWER );
        }
        case PHY_DUTY_CYCLE:
        {
            return LA915_DUTY_CYCLE_ENABLED;
        }
        default:
            return false;
    }
}

void RegionLA915ApplyCFList( ApplyCFListParams_t* applyCFList )
{
    // Size of the optional CF list must be 16 byte
    if( applyCFList->Size != 16 )
    {
        return;
    }

    // Last byte CFListType must be 0x01 to indicate the CFList contains a series of ChMask fields
    if( applyCFList->Payload[15] != 0x01 )
    {
        return;
    }

    // ChMask0 - ChMask4 must be set (every ChMask has 16 bit)
    for( uint8_t chMaskItr = 0, cntPayload = 0; chMaskItr <= 4; chMaskItr++, cntPayload+=2 )
    {
        NvmCtx.ChannelsMask[chMaskItr] = (uint16_t) (0x00FF & applyCFList->Payload[cntPayload]);
        NvmCtx.ChannelsMask[chMaskItr] |= (uint16_t) (applyCFList->Payload[cntPayload+1] << 8);
        if( chMaskItr == 4 )
        {
            NvmCtx.ChannelsMask[chMaskItr] = NvmCtx.ChannelsMask[chMaskItr] & CHANNELS_MASK_500KHZ_MASK;
        }
        // Set the channel mask to the remaining
        NvmCtx.ChannelsMaskRemaining[chMaskItr] &= NvmCtx.ChannelsMask[chMaskItr];
    }
}

bool RegionLA915ChanMaskSet( ChanMaskSetParams_t* chanMaskSet )
{
    switch( chanMaskSet->ChannelsMaskType )
    {
        case CHANNELS_MASK:
        {
            RegionCommonChanMaskCopy( NvmCtx.ChannelsMask, chanMaskSet->ChannelsMaskIn, 6 );

            NvmCtx.ChannelsDefaultMask[4] = NvmCtx.ChannelsDefaultMask[4] & CHANNELS_MASK_500KHZ_MASK;
            NvmCtx.ChannelsDefaultMask[5] = 0x0000;

            for( uint8_t i = 0; i < 6; i++ )
            { // Copy-And the channels mask
                NvmCtx.ChannelsMaskRemaining[i] &= NvmCtx.ChannelsMask[i];
            }
            break;
        }
        case CHANNELS_DEFAULT_MASK:
        {
            RegionCommonChanMaskCopy( NvmCtx.ChannelsDefaultMask, chanMaskSet->ChannelsMaskIn, 6 );
            break;
        }
        default:
            return false;
    }
    return true;
}

void RegionLA915ComputeRxWindowParameters( int8_t datarate, uint8_t minRxSymbols, uint32_t rxError, RxConfigParams_t *rxConfigParams )
{
    double tSymbol = 0.0;

    // Get the datarate, perform a boundary check
    rxConfigParams->Datarate = MIN( datarate, LA915_RX_MAX_DATARATE );
    rxConfigParams->Bandwidth = GetBandwidth( rxConfigParams->Datarate );

    tSymbol = RegionCommonComputeSymbolTimeLoRa( DataratesLA915[rxConfigParams->Datarate], BandwidthsLA915[rxConfigParams->Datarate] );

    RegionCommonComputeRxWindowParameters( tSymbol, minRxSymbols, rxError, Radio.GetWakeupTime( ), &rxConfigParams->WindowTimeout, &rxConfigParams->WindowOffset );
}

bool RegionLA915RxConfig( RxConfigParams_t* rxConfig, int8_t* datarate )
{
    int8_t dr = rxConfig->Datarate;
    uint8_t maxPayload = 0;
    int8_t phyDr = 0;
    uint32_t frequency = rxConfig->Frequency;

    if( Radio.GetStatus( ) != RF_IDLE )
    {
        return false;
    }

    if( rxConfig->RxSlot == RX_SLOT_WIN_1 )
    {
        // Apply window 1 frequency
        frequency = LA915_FIRST_RX1_CHANNEL + ( rxConfig->Channel % 8 ) * LA915_STEPWIDTH_RX1_CHANNEL;
    }

    // Read the physical datarate from the datarates table
    phyDr = DataratesLA915[dr];

    Radio.SetChannel( frequency );

    // Radio configuration
    Radio.SetRxConfig( MODEM_LORA, rxConfig->Bandwidth, phyDr, 1, 0, 8, rxConfig->WindowTimeout, false, 0, false, 0, 0, true, rxConfig->RxContinuous );

    /*if( rxConfig->RepeaterSupport == true )
    {
        maxPayload = MaxPayloadOfDatarateRepeaterDwell0LA915[dr];
    }
    else
    {
        maxPayload = MaxPayloadOfDatarateDwell0LA915[dr];
    }*/
    Radio.SetMaxPayloadLength( MODEM_LORA, maxPayload + LORAMAC_FRAME_PAYLOAD_OVERHEAD_SIZE);

    *datarate = (uint8_t) dr;
    return true;
}

bool RegionLA915TxConfig( TxConfigParams_t* txConfig, int8_t* txPower, TimerTime_t* txTimeOnAir )
{
    int8_t phyDr = DataratesLA915[txConfig->Datarate];
    int8_t txPowerLimited = LimitTxPower( txConfig->TxPower, NvmCtx.Bands[NvmCtx.Channels[txConfig->Channel].Band].TxMaxPower, txConfig->Datarate, NvmCtx.ChannelsMask );
    uint32_t bandwidth = GetBandwidth( txConfig->Datarate );
    int8_t phyTxPower = 0;

    // Calculate physical TX power
    phyTxPower = RegionCommonComputeTxPower( txPowerLimited, txConfig->MaxEirp, txConfig->AntennaGain );

    // Setup the radio frequency
    Radio.SetChannel( NvmCtx.Channels[txConfig->Channel].Frequency );

    Radio.SetTxConfig( MODEM_LORA, phyTxPower, 0, bandwidth, phyDr, 1, 8, false, true, 0, 0, false, 4000 );

    // Setup maximum payload lenght of the radio driver
    Radio.SetMaxPayloadLength( MODEM_LORA, txConfig->PktLen );

    *txTimeOnAir = GetTimeOnAir( txConfig->Datarate, txConfig->PktLen );
    *txPower = txPowerLimited;

    return true;
}

uint8_t RegionLA915LinkAdrReq( LinkAdrReqParams_t* linkAdrReq, int8_t* drOut, int8_t* txPowOut, uint8_t* nbRepOut, uint8_t* nbBytesParsed )
{
    uint8_t status = 0x07;
    RegionCommonLinkAdrParams_t linkAdrParams;
    uint8_t nextIndex = 0;
    uint8_t bytesProcessed = 0;
    uint16_t channelsMask[6] = { 0, 0, 0, 0, 0, 0 };
    GetPhyParams_t getPhy;
    PhyParam_t phyParam;
    RegionCommonLinkAdrReqVerifyParams_t linkAdrVerifyParams;

    // Initialize local copy of channels mask
    RegionCommonChanMaskCopy( channelsMask, NvmCtx.ChannelsMask, 6 );

    while( bytesProcessed < linkAdrReq->PayloadSize )
    {
        nextIndex = RegionCommonParseLinkAdrReq( &( linkAdrReq->Payload[bytesProcessed] ), &linkAdrParams );

        if( nextIndex == 0 )
            break; // break loop, since no more request has been found

        // Update bytes processed
        bytesProcessed += nextIndex;

        // Revert status, as we only check the last ADR request for the channel mask KO
        status = 0x07;

        if( linkAdrParams.ChMaskCtrl == 6 )
        {
            // Enable all 125 kHz channels
            channelsMask[0] = 0xFFFF;
            channelsMask[1] = 0xFFFF;
            channelsMask[2] = 0xFFFF;
            channelsMask[3] = 0xFFFF;
            // Apply chMask to channels 64 to 71
            channelsMask[4] = linkAdrParams.ChMask & CHANNELS_MASK_500KHZ_MASK;
        }
        else if( linkAdrParams.ChMaskCtrl == 7 )
        {
            // Disable all 125 kHz channels
            channelsMask[0] = 0x0000;
            channelsMask[1] = 0x0000;
            channelsMask[2] = 0x0000;
            channelsMask[3] = 0x0000;
            // Apply chMask to channels 64 to 71
            channelsMask[4] = linkAdrParams.ChMask & CHANNELS_MASK_500KHZ_MASK;
        }
        else if( linkAdrParams.ChMaskCtrl == 5 )
        {
            // Start value for comparision
            uint8_t bitMask = 1;

            // cntChannelMask for channelsMask[0] until channelsMask[3]
            uint8_t cntChannelMask = 0;

            // i will be 1, 2, 3, ..., 7
            for( uint8_t i = 0; i <= 7; i++ )
            {
                // 8 MSBs of ChMask are RFU
                // Checking if the ChMask is set, then true
                if( ( ( linkAdrParams.ChMask & 0x00FF ) & ( bitMask << i ) ) != 0 )
                {
                    if( ( i % 2 ) == 0 )
                    {
                        // Enable a bank of 8 125kHz channels, 8 LSBs
                        channelsMask[cntChannelMask] |= 0x00FF;
                        // Enable the corresponding 500kHz channel
                        channelsMask[4] |= ( bitMask << i );
                    }
                    else
                    {
                        // Enable a bank of 8 125kHz channels, 8 MSBs
                        channelsMask[cntChannelMask] |= 0xFF00;
                        // Enable the corresponding 500kHz channel
                        channelsMask[4] |= ( bitMask << i );
                        // cntChannelMask increment for uneven i
                        cntChannelMask++;
                    }
                }
                // ChMask is not set
                else
                {
                    if( ( i % 2 ) == 0 )
                    {
                        // Disable a bank of 8 125kHz channels, 8 LSBs
                        channelsMask[cntChannelMask] &= 0xFF00;
                        // Disable the corresponding 500kHz channel
                        channelsMask[4] &= ~( bitMask << i );
                    }
                    else
                    {
                        // Enable a bank of 8 125kHz channels, 8 MSBs
                        channelsMask[cntChannelMask] &= 0x00FF;
                        // Disable the corresponding 500kHz channel
                        channelsMask[4] &= ~( bitMask << i );
                        // cntChannelMask increment for uneven i
                        cntChannelMask++;
                    }
                }
            }
        }
        else
        {
            channelsMask[linkAdrParams.ChMaskCtrl] = linkAdrParams.ChMask;
        }
    }

    // FCC 15.247 paragraph F mandates to hop on at least 2 125 kHz channels
    if( ( linkAdrParams.Datarate < DR_6 ) && ( RegionCommonCountChannels( channelsMask, 0, 4 ) < 2 ) )
    {
        status &= 0xFE; // Channel mask KO
    }

    // Get the minimum possible datarate
    getPhy.Attribute = PHY_MIN_TX_DR;
    getPhy.UplinkDwellTime = linkAdrReq->UplinkDwellTime;
    phyParam = RegionLA915GetPhyParam( &getPhy );

    linkAdrVerifyParams.Status = status;
    linkAdrVerifyParams.AdrEnabled = linkAdrReq->AdrEnabled;
    linkAdrVerifyParams.Datarate = linkAdrParams.Datarate;
    linkAdrVerifyParams.TxPower = linkAdrParams.TxPower;
    linkAdrVerifyParams.NbRep = linkAdrParams.NbRep;
    linkAdrVerifyParams.CurrentDatarate = linkAdrReq->CurrentDatarate;
    linkAdrVerifyParams.CurrentTxPower = linkAdrReq->CurrentTxPower;
    linkAdrVerifyParams.CurrentNbRep = linkAdrReq->CurrentNbRep;
    linkAdrVerifyParams.NbChannels = LA915_MAX_NB_CHANNELS;
    linkAdrVerifyParams.ChannelsMask = channelsMask;
    linkAdrVerifyParams.MinDatarate = ( int8_t )phyParam.Value;
    linkAdrVerifyParams.MaxDatarate = LA915_TX_MAX_DATARATE;
    linkAdrVerifyParams.Channels = NvmCtx.Channels;
    linkAdrVerifyParams.MinTxPower = LA915_MIN_TX_POWER;
    linkAdrVerifyParams.MaxTxPower = LA915_MAX_TX_POWER;
    linkAdrVerifyParams.Version = linkAdrReq->Version;

    // Verify the parameters and update, if necessary
    status = RegionCommonLinkAdrReqVerifyParams( &linkAdrVerifyParams, &linkAdrParams.Datarate, &linkAdrParams.TxPower, &linkAdrParams.NbRep );

    // Update channelsMask if everything is correct
    if( status == 0x07 )
    {
        // Copy Mask
        RegionCommonChanMaskCopy( NvmCtx.ChannelsMask, channelsMask, 6 );

        NvmCtx.ChannelsMaskRemaining[0] &= NvmCtx.ChannelsMask[0];
        NvmCtx.ChannelsMaskRemaining[1] &= NvmCtx.ChannelsMask[1];
        NvmCtx.ChannelsMaskRemaining[2] &= NvmCtx.ChannelsMask[2];
        NvmCtx.ChannelsMaskRemaining[3] &= NvmCtx.ChannelsMask[3];
        NvmCtx.ChannelsMaskRemaining[4] = NvmCtx.ChannelsMask[4];
        NvmCtx.ChannelsMaskRemaining[5] = NvmCtx.ChannelsMask[5];
    }

    // Update status variables
    *drOut = linkAdrParams.Datarate;
    *txPowOut = linkAdrParams.TxPower;
    *nbRepOut = linkAdrParams.NbRep;
    *nbBytesParsed = bytesProcessed;

    return status;
}

uint8_t RegionLA915RxParamSetupReq( RxParamSetupReqParams_t* rxParamSetupReq )
{
    uint8_t status = 0x07;

    // Verify radio frequency
    if( VerifyRfFreq( rxParamSetupReq->Frequency ) == false )
    {
        status &= 0xFE; // Channel frequency KO
    }

    // Verify datarate
    if( RegionCommonValueInRange( rxParamSetupReq->Datarate, LA915_RX_MIN_DATARATE, LA915_RX_MAX_DATARATE ) == false )
    {
        status &= 0xFD; // Datarate KO
    }
    if( ( rxParamSetupReq->Datarate == DR_7 ) ||
        ( rxParamSetupReq->Datarate > DR_13 ) )
    {
        status &= 0xFD; // Datarate KO
    }

    // Verify datarate offset
    if( RegionCommonValueInRange( rxParamSetupReq->DrOffset, LA915_MIN_RX1_DR_OFFSET, LA915_MAX_RX1_DR_OFFSET ) == false )
    {
        status &= 0xFB; // Rx1DrOffset range KO
    }

    return status;
}

uint8_t RegionLA915NewChannelReq( NewChannelReqParams_t* newChannelReq )
{
    // Datarate and frequency KO
    return 0;
}

int8_t RegionLA915TxParamSetupReq( TxParamSetupReqParams_t* txParamSetupReq )
{
    // Accept the request
    return 0;
}

uint8_t RegionLA915DlChannelReq( DlChannelReqParams_t* dlChannelReq )
{
    return 0;
}

int8_t RegionLA915AlternateDr( int8_t currentDr, AlternateDrType_t type )
{
    static int8_t trialsCount = 0;

    // Re-enable 500 kHz default channels
    NvmCtx.ChannelsMask[4] = CHANNELS_MASK_500KHZ_MASK;

    if( ( trialsCount & 0x01 ) == 0x01 )
    {
        currentDr = DR_6;
    }
    else
    {
        currentDr = DR_2;
    }
    trialsCount++;
    return currentDr;
}

/*void RegionLA915CalcBackOff( CalcBackOffParams_t* calcBackOff )
{
    RegionCommonCalcBackOffParams_t calcBackOffParams;

    calcBackOffParams.Channels = NvmCtx.Channels;
    calcBackOffParams.Bands = NvmCtx.Bands;
    calcBackOffParams.LastTxIsJoinRequest = calcBackOff->LastTxIsJoinRequest;
    calcBackOffParams.Joined = calcBackOff->Joined;
    calcBackOffParams.DutyCycleEnabled = calcBackOff->DutyCycleEnabled;
    calcBackOffParams.Channel = calcBackOff->Channel;
    calcBackOffParams.ElapsedTime = calcBackOff->ElapsedTime;
    calcBackOffParams.TxTimeOnAir = calcBackOff->TxTimeOnAir;

    RegionCommonCalcBackOff( &calcBackOffParams );
}*/

LoRaMacStatus_t RegionLA915NextChannel( NextChanParams_t* nextChanParams, uint8_t* channel, TimerTime_t* time, TimerTime_t* aggregatedTimeOff )
{
    uint8_t nbEnabledChannels = 0;
    uint8_t delayTx = 0;
    uint8_t enabledChannels[LA915_MAX_NB_CHANNELS] = { 0 };
    TimerTime_t nextTxDelay = 0;

    // Count 125kHz channels
    if( RegionCommonCountChannels( NvmCtx.ChannelsMaskRemaining, 0, 4 ) == 0 )
    { // Reactivate default channels
        RegionCommonChanMaskCopy( NvmCtx.ChannelsMaskRemaining, NvmCtx.ChannelsMask, 4  );
    }
    // Check other channels
    if( nextChanParams->Datarate >= DR_6 )
    {
        if( ( NvmCtx.ChannelsMaskRemaining[4] & CHANNELS_MASK_500KHZ_MASK ) == 0 )
        {
            NvmCtx.ChannelsMaskRemaining[4] = NvmCtx.ChannelsMask[4];
        }
    }

    TimerTime_t elapsed = TimerGetElapsedTime( nextChanParams->LastAggrTx );
    if( ( nextChanParams->LastAggrTx == 0 ) || ( nextChanParams->AggrTimeOff <= elapsed ) )
    {
        // Reset Aggregated time off
        *aggregatedTimeOff = 0;

        // Update bands Time OFF
        nextTxDelay = RegionCommonUpdateBandTimeOff( nextChanParams->Joined, NvmCtx.Bands, LA915_MAX_NB_BANDS,nextChanParams->DutyCycleEnabled , nextChanParams->LastTxIsJoinRequest, nextChanParams->ElapsedTimeSinceStartUp,GetTimeOnAir( nextChanParams->Datarate, nextChanParams->PktLen ));

        // Search how many channels are enabled
        nbEnabledChannels = CountNbOfEnabledChannels( nextChanParams->Datarate,
                                                      NvmCtx.ChannelsMaskRemaining, NvmCtx.Channels,
                                                      NvmCtx.Bands, enabledChannels, &delayTx );
    }
    else
    {
        delayTx++;
        nextTxDelay = nextChanParams->AggrTimeOff - elapsed;
    }

    if( nbEnabledChannels > 0 )
    {
        // We found a valid channel
        *channel = enabledChannels[randr( 0, nbEnabledChannels - 1 )];
        // Disable the channel in the mask
        RegionCommonChanDisable( NvmCtx.ChannelsMaskRemaining, *channel, LA915_MAX_NB_CHANNELS - 8 );

        *time = 0;
        return LORAMAC_STATUS_OK;
    }
    else
    {
        if( delayTx > 0 )
        {
            // Delay transmission due to AggregatedTimeOff or to a band time off
            *time = nextTxDelay;
            return LORAMAC_STATUS_DUTYCYCLE_RESTRICTED;
        }
        // Datarate not supported by any channel
        *time = 0;
        return LORAMAC_STATUS_NO_CHANNEL_FOUND;
    }
}

LoRaMacStatus_t RegionLA915ChannelAdd( ChannelAddParams_t* channelAdd )
{
    return LORAMAC_STATUS_PARAMETER_INVALID;
}

bool RegionLA915ChannelsRemove( ChannelRemoveParams_t* channelRemove  )
{
    return LORAMAC_STATUS_PARAMETER_INVALID;
}

uint8_t RegionLA915ApplyDrOffset( uint8_t downlinkDwellTime, int8_t dr, int8_t drOffset )
{
    int8_t datarate = DatarateOffsetsLA915[dr][drOffset];

    if( datarate < 0 )
    {
        if( downlinkDwellTime == 0 )
        {
            datarate = LA915_TX_MIN_DATARATE;
        }
        else
        {
            datarate = LA915_DWELL_LIMIT_DATARATE;
        }
    }
    return datarate;
}

void RegionLA915RxBeaconSetup( RxBeaconSetup_t* rxBeaconSetup, uint8_t* outDr )
{
    RegionCommonRxBeaconSetupParams_t regionCommonRxBeaconSetup;

    regionCommonRxBeaconSetup.Datarates = DataratesLA915;
    regionCommonRxBeaconSetup.Frequency = rxBeaconSetup->Frequency;
    regionCommonRxBeaconSetup.BeaconSize = LA915_BEACON_SIZE;
    regionCommonRxBeaconSetup.BeaconDatarate = LA915_BEACON_CHANNEL_DR;
    regionCommonRxBeaconSetup.BeaconChannelBW = LA915_BEACON_CHANNEL_BW;
    regionCommonRxBeaconSetup.RxTime = rxBeaconSetup->RxTime;
    regionCommonRxBeaconSetup.SymbolTimeout = rxBeaconSetup->SymbolTimeout;

    RegionCommonRxBeaconSetup( &regionCommonRxBeaconSetup );

    // Store downlink datarate
    *outDr = LA915_BEACON_CHANNEL_DR;
}
#endif