/*
 *  TAP-Windows -- A kernel driver to provide virtual tap
 *                 device functionality on Windows.
 *
 *  This code was inspired by the CIPE-Win32 driver by Damion K. Wilson.
 *
 *  This source code is Copyright (C) 2002-2014 OpenVPN Technologies, Inc.,
 *  and is released under the GPL version 2 (see below).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

//
// Include files.
//

#include "tap-windows.h"

NDIS_OID TAPSupportedOids[] =
{
        OID_GEN_HARDWARE_STATUS,
        OID_GEN_TRANSMIT_BUFFER_SPACE,
        OID_GEN_RECEIVE_BUFFER_SPACE,
        OID_GEN_TRANSMIT_BLOCK_SIZE,
        OID_GEN_RECEIVE_BLOCK_SIZE,
        OID_GEN_VENDOR_ID,
        OID_GEN_VENDOR_DESCRIPTION,
        OID_GEN_VENDOR_DRIVER_VERSION,
        OID_GEN_CURRENT_PACKET_FILTER,
        OID_GEN_CURRENT_LOOKAHEAD,
        OID_GEN_DRIVER_VERSION,
        OID_GEN_MAXIMUM_TOTAL_SIZE,
        OID_GEN_XMIT_OK,
        OID_GEN_RCV_OK,
        OID_GEN_STATISTICS,
#ifdef IMPLEMENT_OPTIONAL_OIDS
        OID_GEN_TRANSMIT_QUEUE_LENGTH,       // Optional
#endif // IMPLEMENT_OPTIONAL_OIDS
        OID_GEN_LINK_PARAMETERS,
        OID_GEN_INTERRUPT_MODERATION,
        OID_GEN_MEDIA_SUPPORTED,
        OID_GEN_MEDIA_IN_USE,
        OID_GEN_MAXIMUM_SEND_PACKETS,
        OID_GEN_XMIT_ERROR,
        OID_GEN_RCV_ERROR,
        OID_GEN_RCV_NO_BUFFER,
        OID_802_3_PERMANENT_ADDRESS,
        OID_802_3_CURRENT_ADDRESS,
        OID_802_3_MULTICAST_LIST,
        OID_802_3_MAXIMUM_LIST_SIZE,
        OID_802_3_RCV_ERROR_ALIGNMENT,
        OID_802_3_XMIT_ONE_COLLISION,
        OID_802_3_XMIT_MORE_COLLISIONS,
#ifdef IMPLEMENT_OPTIONAL_OIDS
        OID_802_3_XMIT_DEFERRED,             // Optional
        OID_802_3_XMIT_MAX_COLLISIONS,       // Optional
        OID_802_3_RCV_OVERRUN,               // Optional
        OID_802_3_XMIT_UNDERRUN,             // Optional
        OID_802_3_XMIT_HEARTBEAT_FAILURE,    // Optional
        OID_802_3_XMIT_TIMES_CRS_LOST,       // Optional
        OID_802_3_XMIT_LATE_COLLISIONS,      // Optional
        OID_PNP_CAPABILITIES,                // Optional
#endif // IMPLEMENT_OPTIONAL_OIDS
};

//======================================================================
// TAP NDIS 6 Miniport Callbacks
//======================================================================

// Returns with reference count initialized to one.
PTAP_ADAPTER_CONTEXT
tapAdapterContextAllocate(
    __in NDIS_HANDLE        MiniportAdapterHandle
)
{
    PTAP_ADAPTER_CONTEXT   adapter = NULL;

    adapter = (PTAP_ADAPTER_CONTEXT )NdisAllocateMemoryWithTagPriority(
        GlobalData.NdisDriverHandle,
        sizeof(TAP_ADAPTER_CONTEXT),
        TAP_TAG_ADAPTER,
        NormalPoolPriority
        );

    if(adapter)
    {
        NdisZeroMemory(adapter,sizeof(TAP_ADAPTER_CONTEXT));

        // Add initial reference. Normally removed in AdapterHalt.
        adapter->RefCount = 1;

        adapter->MiniportAdapterHandle = MiniportAdapterHandle;

        // Safe for multiple removes.
        NdisInitializeListHead(&adapter->AdapterListLink);
    }

    return adapter;
}

VOID
tapReadPermanentAddress(
    __in PTAP_ADAPTER_CONTEXT   Adapter,
    __in NDIS_HANDLE            ConfigurationHandle,
    __out MACADDR               PeranentAddress
    )
{
    NDIS_STATUS status;
    NDIS_CONFIGURATION_PARAMETER *configParameter;
    NDIS_STRING macKey = NDIS_STRING_CONST("MAC");
    ANSI_STRING macString;
    BOOLEAN macFromRegistry;

    // Read MAC parameter from registry.
    NdisReadConfiguration(
        &status,
        &configParameter,
        ConfigurationHandle,
        &macKey,
        NdisParameterString
        );

    if (status == NDIS_STATUS_SUCCESS)
    {
        if (configParameter->ParameterType == NdisParameterString)
        {
            if (RtlUnicodeStringToAnsiString(
                    &macString,
                    &configParameter->ParameterData.StringData,
                    TRUE) == STATUS_SUCCESS
                    )
            {
                macFromRegistry = ParseMAC (Adapter->PermanentAddress, macString.Buffer);
                RtlFreeAnsiString (&macString);
            }
        }
    }

    // BUGBUG!!! MAC address is all zeroes in registry...
    macFromRegistry = FALSE;

    if(!macFromRegistry)
    {
        //
        // There is no (valid) address stashed in the registry parameter.
        //
        // Make up a dummy mac address based on the ANSI representation of the
        // NetCfgInstanceId GUID.
        //
        GenerateRandomMac(Adapter->PermanentAddress, MINIPORT_INSTANCE_ID(Adapter));
    }
}

NDIS_STATUS
tapReadConfiguration(
    __in PTAP_ADAPTER_CONTEXT     Adapter
    )
{
    NDIS_STATUS                 status = NDIS_STATUS_SUCCESS;
    NDIS_CONFIGURATION_OBJECT   configObject;
    NDIS_HANDLE                 configHandle;

    DEBUGP (("[TAP] --> tapReadConfiguration\n"));

    //
    // Setup defaults in case configuration cannot be opened.
    //
    Adapter->MtuSize = ETHERNET_MTU;
    Adapter->MediaStateAlwaysConnected = FALSE;
    Adapter->MediaState = FALSE;
    Adapter->AllowNonAdmin = FALSE;
    //
    // Open the registry for this adapter to read advanced
    // configuration parameters stored by the INF file.
    //
    NdisZeroMemory(&configObject, sizeof(configObject));

    {C_ASSERT(sizeof(configObject) >= NDIS_SIZEOF_CONFIGURATION_OBJECT_REVISION_1);}
    configObject.Header.Type = NDIS_OBJECT_TYPE_CONFIGURATION_OBJECT;
    configObject.Header.Size = NDIS_SIZEOF_CONFIGURATION_OBJECT_REVISION_1;
    configObject.Header.Revision = NDIS_CONFIGURATION_OBJECT_REVISION_1;

    configObject.NdisHandle = Adapter->MiniportAdapterHandle;
    configObject.Flags = 0;

    status = NdisOpenConfigurationEx(
                &configObject,
                &configHandle
                );

    // Read on the opened configuration handle.
    if(status == NDIS_STATUS_SUCCESS)
    {
        NDIS_CONFIGURATION_PARAMETER *configParameter;
        NDIS_STRING mkey = NDIS_STRING_CONST("NetCfgInstanceId");

        //
        // Read NetCfgInstanceId from the registry.
        // ------------------------------------
        // NetCfgInstanceId is required to create device and associated
        // symbolic link for the adapter device.
        //
        // NetCfgInstanceId is  a GUID string provided by NDIS that identifies
        // the adapter instance. An example is:
        // 
        //    NetCfgInstanceId={410EB49D-2381-4FE7-9B36-498E22619DF0}
        //
        // Other names are derived from NetCfgInstanceId. For example, MiniportName:
        //
        //    MiniportName=\DEVICE\{410EB49D-2381-4FE7-9B36-498E22619DF0}
        //
        NdisReadConfiguration (
            &status,
            &configParameter,
            configHandle,
            &mkey,
            NdisParameterString
            );

        if (status == NDIS_STATUS_SUCCESS)
        {
            if (configParameter->ParameterType == NdisParameterString)
            {
                DEBUGP (("[TAP] NdisReadConfiguration (NetCfgInstanceId=%wZ)\n",
                    &configParameter->ParameterData.StringData ));

                // Save NetCfgInstanceId as UNICODE_STRING.
                Adapter->NetCfgInstanceId.Length = Adapter->NetCfgInstanceId.MaximumLength
                    = configParameter->ParameterData.StringData.Length;

                Adapter->NetCfgInstanceId.Buffer = Adapter->NetCfgInstanceIdBuffer;

                NdisMoveMemory(
                    Adapter->NetCfgInstanceId.Buffer, 
                    configParameter->ParameterData.StringData.Buffer,
                    Adapter->NetCfgInstanceId.Length
                    );

                // Save NetCfgInstanceId as ANSI_STRING as well.
                if (RtlUnicodeStringToAnsiString (
                        &Adapter->NetCfgInstanceIdAnsi,
                        &configParameter->ParameterData.StringData,
                        TRUE) != STATUS_SUCCESS
                    )
                {
                    DEBUGP (("[TAP] NetCfgInstanceId ANSI name conversion failed\n"));
                    status = NDIS_STATUS_RESOURCES;
                }
            }
            else
            {
                DEBUGP (("[TAP] NetCfgInstanceId has invalid type\n"));
                status = NDIS_STATUS_INVALID_DATA;
            }
        }
        else
        {
            DEBUGP (("[TAP] NetCfgInstanceId failed\n"));
            status = NDIS_STATUS_INVALID_DATA;
        }

        if (status == NDIS_STATUS_SUCCESS)
        {
            NDIS_STATUS localStatus;    // Use default if these fail.
            NDIS_CONFIGURATION_PARAMETER *configParameter;
            NDIS_STRING mtuKey = NDIS_STRING_CONST("MTU");
            NDIS_STRING mediaStatusKey = NDIS_STRING_CONST("MediaStatus");
#if ENABLE_NONADMIN
            NDIS_STRING allowNonAdminKey = NDIS_STRING_CONST("AllowNonAdmin");
#endif

            // Read MTU from the registry.
            NdisReadConfiguration (
                &localStatus,
                &configParameter,
                configHandle,
                &mtuKey,
                NdisParameterInteger
                );

            if (localStatus == NDIS_STATUS_SUCCESS)
            {
                if (configParameter->ParameterType == NdisParameterInteger)
                {
                    int mtu = configParameter->ParameterData.IntegerData;

                    // Sanity check
                    if (mtu < MINIMUM_MTU)
                    {
                        mtu = MINIMUM_MTU;
                    }
                    else if (mtu > MAXIMUM_MTU)
                    {
                        mtu = MAXIMUM_MTU;
                    }

                    Adapter->MtuSize = mtu;
                }
            }

            // Read MediaStatus setting from registry.
            NdisReadConfiguration (
                &localStatus,
                &configParameter,
                configHandle,
                &mediaStatusKey,
                NdisParameterInteger
                );

            if (localStatus == NDIS_STATUS_SUCCESS)
            {
                if (configParameter->ParameterType == NdisParameterInteger)
                {
		          Adapter->MediaStateAlwaysConnected = TRUE;
		          Adapter->MediaState = TRUE;
                }
            }

            // Read MAC PermanentAddress setting from registry.
            tapReadPermanentAddress(
                Adapter,
                configHandle,
                Adapter->PermanentAddress
                );

            DEBUGP (("[%s] Using MAC PermanentAddress %x:%x:%x:%x:%x:%x\n",
                MINIPORT_INSTANCE_ID (Adapter),
                Adapter->PermanentAddress[0],
                Adapter->PermanentAddress[1],
                Adapter->PermanentAddress[2],
                Adapter->PermanentAddress[3],
                Adapter->PermanentAddress[4],
                Adapter->PermanentAddress[5])
                );

            // Now seed the current MAC address with the permanent address.
            COPY_MAC(Adapter->CurrentAddress, Adapter->PermanentAddress);

            DEBUGP (("[%s] Using MAC CurrentAddress %x:%x:%x:%x:%x:%x\n",
                MINIPORT_INSTANCE_ID (Adapter),
                Adapter->CurrentAddress[0],
                Adapter->CurrentAddress[1],
                Adapter->CurrentAddress[2],
                Adapter->CurrentAddress[3],
                Adapter->CurrentAddress[4],
                Adapter->CurrentAddress[5])
                );

            // Read optional AllowNonAdmin setting from registry.
#if ENABLE_NONADMIN
            NdisReadConfiguration (
                &localStatus,
                &configParameter,
                configHandle,
                &allowNonAdminKey,
                NdisParameterInteger
                );

            if (localStatus == NDIS_STATUS_SUCCESS)
            {
                if (configParameter->ParameterType == NdisParameterInteger)
                {
                    Adapter->AllowNonAdmin = TRUE;
                }
            }
#endif
        }

        // Close the configuration handle.
        NdisCloseConfiguration(configHandle);
    }
    else
    {
        DEBUGP (("[TAP] Couldn't open adapter registry\n"));
    }

    DEBUGP (("[TAP] <-- tapReadConfiguration; status = %8.8X\n",status));

    return status;
}

// Returns with added reference on adapter context.
PTAP_ADAPTER_CONTEXT
tapAdapterContextFromDeviceObject(
    __in PDEVICE_OBJECT DeviceObject
    )
{
    LOCK_STATE              lockState;

    // Acquire global adapter list lock.
    NdisAcquireReadWriteLock(
        &GlobalData.Lock,
        FALSE,      // Acquire for read
        &lockState
        );

    if (!IsListEmpty(&GlobalData.AdapterList))
    {
        PLIST_ENTRY             entry = GlobalData.AdapterList.Flink;
        PTAP_ADAPTER_CONTEXT    adapter;

        while (entry != &GlobalData.AdapterList)
        {
            adapter = CONTAINING_RECORD(entry, TAP_ADAPTER_CONTEXT, AdapterListLink);

            // Match on DeviceObject
            if(adapter->DeviceObject == DeviceObject )
            {
                // Add reference to adapter context.
                tapAdapterContextReference(adapter);

                // Release global adapter list lock.
                NdisReleaseReadWriteLock(&GlobalData.Lock,&lockState);

                return adapter;
            }
        }
    }

    // Release global adapter list lock.
    NdisReleaseReadWriteLock(&GlobalData.Lock,&lockState);

    return (PTAP_ADAPTER_CONTEXT )NULL;
}


NDIS_STATUS
AdapterCreate(
    __in  NDIS_HANDLE                         MiniportAdapterHandle,
    __in  NDIS_HANDLE                         MiniportDriverContext,
    __in  PNDIS_MINIPORT_INIT_PARAMETERS      MiniportInitParameters
    )
{
    PTAP_ADAPTER_CONTEXT    adapter = NULL;
    NDIS_STATUS             status;

    UNREFERENCED_PARAMETER(MiniportDriverContext);
    UNREFERENCED_PARAMETER(MiniportInitParameters);

    DEBUGP (("[TAP] --> AdapterCreate\n"));

    do
    {
        NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES regAttributes = {0};
        NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES genAttributes = {0};
        NDIS_PNP_CAPABILITIES pnpCapabilities = {0};

        //
        // Allocate adapter context structure and initialize all the
        // memory resources for sending and receiving packets.
        //
        // Returns with reference count initialized to one.
        //
        adapter = tapAdapterContextAllocate(MiniportAdapterHandle);

        if(adapter == NULL)
        {
            DEBUGP (("[TAP] Couldn't allocate adapter memory\n"));
            status = NDIS_STATUS_RESOURCES;
            break;
        }

        //
        // First read adapter configuration from registry.
        // -----------------------------------------------
        // Subsequent device registration will fail if NetCfgInstanceId
        // has not been successfully read.
        //
        status = tapReadConfiguration(adapter);

        //
        // Set the registration attributes.
        //
        {C_ASSERT(sizeof(regAttributes) >= NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1);}
        regAttributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
        regAttributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
        regAttributes.Header.Revision = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;

        regAttributes.MiniportAdapterContext = adapter;
        regAttributes.AttributeFlags = TAP_ADAPTER_ATTRIBUTES_FLAGS;

        regAttributes.CheckForHangTimeInSeconds = TAP_ADAPTER_CHECK_FOR_HANG_TIME_IN_SECONDS;
        regAttributes.InterfaceType = TAP_INTERFACE_TYPE;

        //NDIS_DECLARE_MINIPORT_ADAPTER_CONTEXT(TAP_ADAPTER_CONTEXT);
        status = NdisMSetMiniportAttributes(
                    MiniportAdapterHandle,
                    (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&regAttributes
                    );

        if (status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP (("[TAP] NdisSetOptionalHandlers failed; Status 0x%08x\n",status));
            break;
        }

        //
        // Next, set the general attributes.
        //
        {C_ASSERT(sizeof(genAttributes) >= NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1);}
        genAttributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
        genAttributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1;
        genAttributes.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1;

        //
        // Specify the medium type that the NIC can support but not
        // necessarily the medium type that the NIC currently uses.
        //
        genAttributes.MediaType = TAP_MEDIUM_TYPE;

        //
        // Specifiy medium type that the NIC currently uses.
        //
        genAttributes.PhysicalMediumType = TAP_PHYSICAL_MEDIUM;

        //
        // Specifiy the maximum network frame size, in bytes, that the NIC
        // supports excluding the header.
        //
        genAttributes.MtuSize = TAP_FRAME_MAX_DATA_SIZE;
        genAttributes.MaxXmitLinkSpeed = TAP_XMIT_SPEED;
        genAttributes.XmitLinkSpeed = TAP_XMIT_SPEED;
        genAttributes.MaxRcvLinkSpeed = TAP_RECV_SPEED;
        genAttributes.RcvLinkSpeed = TAP_RECV_SPEED;

        // BUGBUG!!! Implement me!!!
        //genAttributes.MediaConnectState = HWGetMediaConnectStatus(adapter);
        genAttributes.MediaConnectState = MediaConnectStateDisconnected;

        genAttributes.MediaDuplexState = MediaDuplexStateFull;

        //
        // The maximum number of bytes the NIC can provide as lookahead data.
        // If that value is different from the size of the lookahead buffer
        // supported by bound protocols, NDIS will call MiniportOidRequest to
        // set the size of the lookahead buffer provided by the miniport driver
        // to the minimum of the miniport driver and protocol(s) values. If the
        // driver always indicates up full packets with
        // NdisMIndicateReceiveNetBufferLists, it should set this value to the
        // maximum total frame size, which excludes the header.
        //
        // Upper-layer drivers examine lookahead data to determine whether a
        // packet that is associated with the lookahead data is intended for
        // one or more of their clients. If the underlying driver supports
        // multipacket receive indications, bound protocols are given full net
        // packets on every indication. Consequently, this value is identical
        // to that returned for OID_GEN_RECEIVE_BLOCK_SIZE.
        //
        genAttributes.LookaheadSize = TAP_MAX_LOOKAHEAD;
        genAttributes.MacOptions = TAP_MAC_OPTIONS;
        genAttributes.SupportedPacketFilters = TAP_SUPPORTED_FILTERS;

        //
        // The maximum number of multicast addresses the NIC driver can manage.
        // This list is global for all protocols bound to (or above) the NIC.
        // Consequently, a protocol can receive NDIS_STATUS_MULTICAST_FULL from
        // the NIC driver when attempting to set the multicast address list,
        // even if the number of elements in the given list is less than the
        // number originally returned for this query.
        //
        genAttributes.MaxMulticastListSize = TAP_MAX_MCAST_LIST;
        genAttributes.MacAddressLength = MACADDR_SIZE;

        //
        // Return the MAC address of the NIC burnt in the hardware.
        //
        COPY_MAC(genAttributes.PermanentMacAddress, adapter->PermanentAddress);

        //
        // Return the MAC address the NIC is currently programmed to use. Note
        // that this address could be different from the permananent address as
        // the user can override using registry. Read NdisReadNetworkAddress
        // doc for more info.
        //
        COPY_MAC(genAttributes.CurrentMacAddress, adapter->CurrentAddress);

        genAttributes.RecvScaleCapabilities = NULL;
        genAttributes.AccessType = TAP_ACCESS_TYPE;
        genAttributes.DirectionType = TAP_DIRECTION_TYPE;
        genAttributes.ConnectionType = TAP_CONNECTION_TYPE;
        genAttributes.IfType = TAP_IFTYPE;
        genAttributes.IfConnectorPresent = TAP_HAS_PHYSICAL_CONNECTOR;
        genAttributes.SupportedStatistics = TAP_SUPPORTED_STATISTICS;
        genAttributes.SupportedPauseFunctions = NdisPauseFunctionsUnsupported; // IEEE 802.3 pause frames 
        genAttributes.DataBackFillSize = 0;
        genAttributes.ContextBackFillSize = 0;

        //
        // The SupportedOidList is an array of OIDs for objects that the
        // underlying driver or its NIC supports.  Objects include general,
        // media-specific, and implementation-specific objects. NDIS forwards a
        // subset of the returned list to protocols that make this query. That
        // is, NDIS filters any supported statistics OIDs out of the list
        // because protocols never make statistics queries.
        //
        genAttributes.SupportedOidList = TAPSupportedOids;
        genAttributes.SupportedOidListLength = sizeof(TAPSupportedOids);
        genAttributes.AutoNegotiationFlags = NDIS_LINK_STATE_DUPLEX_AUTO_NEGOTIATED;

        //
        // Set power management capabilities
        //
        NdisZeroMemory(&pnpCapabilities, sizeof(pnpCapabilities));
        pnpCapabilities.WakeUpCapabilities.MinMagicPacketWakeUp = NdisDeviceStateUnspecified;
        pnpCapabilities.WakeUpCapabilities.MinPatternWakeUp = NdisDeviceStateUnspecified;
        genAttributes.PowerManagementCapabilities = &pnpCapabilities;

        status = NdisMSetMiniportAttributes(
                    MiniportAdapterHandle,
                    (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&genAttributes
                    );

        if (status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP (("[TAP] NdisMSetMiniportAttributes failed; Status 0x%08x\n",status));
            break;
        }

        //
        // Create the Win32 device I/O interface.
        //
        status = CreateTapDevice(adapter);

        if (status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP (("[TAP] CreateTapDevice failed; Status 0x%08x\n",status));
            break;
        }
    } while(FALSE);

    if(status != NDIS_STATUS_SUCCESS)
    {
        if(adapter != NULL)
        {
            //
            // Remove reference when adapter context was allocated
            // ---------------------------------------------------
            // This should result in freeing adapter context memory
            // and assiciated resources.
            //
            tapAdapterContextDereference(adapter);
            adapter = NULL;
        }
    }

    DEBUGP (("[TAP] <-- AdapterCreate; status = %8.8X\n",status));

    return status;
}

VOID
AdapterHalt(
    __in  NDIS_HANDLE             MiniportAdapterContext,
    __in  NDIS_HALT_ACTION        HaltAction
    )
/*++

Routine Description:

    Halt handler is called when NDIS receives IRP_MN_STOP_DEVICE,
    IRP_MN_SUPRISE_REMOVE or IRP_MN_REMOVE_DEVICE requests from the PNP
    manager. Here, the driver should free all the resources acquired in
    MiniportInitialize and stop access to the hardware. NDIS will not submit
    any further request once this handler is invoked.

    1) Free and unmap all I/O resources.
    2) Disable interrupt and deregister interrupt handler.
    3) Deregister shutdown handler regsitered by
        NdisMRegisterAdapterShutdownHandler .
    4) Cancel all queued up timer callbacks.
    5) Finally wait indefinitely for all the outstanding receive
        packets indicated to the protocol to return.

    MiniportHalt runs at IRQL = PASSIVE_LEVEL.


Arguments:

    MiniportAdapterContext  Pointer to the Adapter
    HaltAction  The reason for halting the adapter

Return Value:

    None.

--*/
{
    PTAP_ADAPTER_CONTEXT   adapter = (PTAP_ADAPTER_CONTEXT )MiniportAdapterContext;

    UNREFERENCED_PARAMETER(HaltAction);

    DEBUGP (("[TAP] --> AdapterHalt\n"));

    //
    // Destroy the TAP Win32 device.
    //
    DestroyTapDevice(adapter);

    //
    // Remove initial reference added in AdapterCreate.
    // ------------------------------------------------
    // This should result in freeing adapter context memory
    // and resources allocated in AdapterCreate.
    //
    tapAdapterContextDereference(adapter);
    adapter = NULL;

    DEBUGP (("[TAP] <-- AdapterHalt\n"));
}

NDIS_STATUS
AdapterPause(
    __in  NDIS_HANDLE                       MiniportAdapterContext,
    __in  PNDIS_MINIPORT_PAUSE_PARAMETERS   PauseParameters
    )
{
    PTAP_ADAPTER_CONTEXT   adapter = (PTAP_ADAPTER_CONTEXT )MiniportAdapterContext;
    NDIS_STATUS    status;

    UNREFERENCED_PARAMETER(PauseParameters);

    status = NDIS_STATUS_SUCCESS;

    return status;
}

NDIS_STATUS
AdapterRestart(
    __in  NDIS_HANDLE                             MiniportAdapterContext,
    __in  PNDIS_MINIPORT_RESTART_PARAMETERS       RestartParameters
    )
{
    PTAP_ADAPTER_CONTEXT   adapter = (PTAP_ADAPTER_CONTEXT )MiniportAdapterContext;
    NDIS_STATUS    status;

    UNREFERENCED_PARAMETER(RestartParameters);

    status = NDIS_STATUS_FAILURE;

    return status;
}

VOID
tapSendNetBufferListsComplete(
    __in PTAP_ADAPTER_CONTEXT       Adapter,
    __in PNET_BUFFER_LIST   NetBufferLists,
    __in NDIS_STATUS        SendCompletionStatus,
    __in BOOLEAN            DispatchLevel
    )
{
    PNET_BUFFER_LIST    netBufferList;
    PNET_BUFFER_LIST    nextNetBufferList = NULL;
    ULONG               sendCompleteFlags = 0;

    for (
        netBufferList = NetBufferLists;
        netBufferList!= NULL;
        netBufferList = nextNetBufferList
        )
    {
        nextNetBufferList = NET_BUFFER_LIST_NEXT_NBL(netBufferList);

        NET_BUFFER_LIST_STATUS(netBufferList) = SendCompletionStatus;

        netBufferList = nextNetBufferList;
    }

    if(DispatchLevel)
    {
        sendCompleteFlags |= NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL;
    }

    // Complete the NBLs
    NdisMSendNetBufferListsComplete(
        Adapter->MiniportAdapterHandle,
        NetBufferLists,
        sendCompleteFlags
        );
}

VOID
AdapterSendNetBufferLists(
    __in  NDIS_HANDLE             MiniportAdapterContext,
    __in  PNET_BUFFER_LIST        NetBufferLists,
    __in  NDIS_PORT_NUMBER        PortNumber,
    __in  ULONG                   SendFlags
    )
/*++

Routine Description:

    Send Packet Array handler. Called by NDIS whenever a protocol
    bound to our miniport sends one or more packets.

    The input packet descriptor pointers have been ordered according
    to the order in which the packets should be sent over the network
    by the protocol driver that set up the packet array. The NDIS
    library preserves the protocol-determined ordering when it submits
    each packet array to MiniportSendPackets

    As a deserialized driver, we are responsible for holding incoming send
    packets in our internal queue until they can be transmitted over the
    network and for preserving the protocol-determined ordering of packet
    descriptors incoming to its MiniportSendPackets function.
    A deserialized miniport driver must complete each incoming send packet
    with NdisMSendComplete, and it cannot call NdisMSendResourcesAvailable.

    Runs at IRQL <= DISPATCH_LEVEL

Arguments:

    MiniportAdapterContext      Pointer to our adapter
    NetBufferLists              Head of a list of NBLs to send
    PortNumber                  A miniport adapter port.  Default is 0.
    SendFlags                   Additional flags for the send operation

Return Value:

    None.  Write status directly into each NBL with the NET_BUFFER_LIST_STATUS
    macro.

--*/
{
    PTAP_ADAPTER_CONTEXT   adapter = (PTAP_ADAPTER_CONTEXT )MiniportAdapterContext;

    UNREFERENCED_PARAMETER(NetBufferLists);
    UNREFERENCED_PARAMETER(PortNumber);
    UNREFERENCED_PARAMETER(SendFlags);

    ASSERT(PortNumber == 0); // Only the default port is supported

    // For now just complete all NBLs...
    tapSendNetBufferListsComplete(
        adapter,
        NetBufferLists,
        NDIS_STATUS_SUCCESS,
        (SendFlags & NDIS_SEND_FLAGS_DISPATCH_LEVEL)
        );
}

VOID
AdapterCancelSend(
    __in  NDIS_HANDLE             MiniportAdapterContext,
    __in  PVOID                   CancelId
    )
{
    PTAP_ADAPTER_CONTEXT   adapter = (PTAP_ADAPTER_CONTEXT )MiniportAdapterContext;

    //
    // This miniport completes its sends quickly, so it isn't strictly
    // neccessary to implement MiniportCancelSend.
    //
    // If we did implement it, we'd have to walk the Adapter->SendWaitList
    // and look for any NB that points to a NBL where the CancelId matches
    // NDIS_GET_NET_BUFFER_LIST_CANCEL_ID(Nbl).  For any NB that so matches,
    // we'd remove the NB from the SendWaitList and set the NBL's status to
    // NDIS_STATUS_SEND_ABORTED, then complete the NBL.
    //
}

VOID
AdapterReturnNetBufferLists(
    __in  NDIS_HANDLE             MiniportAdapterContext,
    __in  PNET_BUFFER_LIST        NetBufferLists,
    __in  ULONG                   ReturnFlags
    )
{
    PTAP_ADAPTER_CONTEXT   adapter = (PTAP_ADAPTER_CONTEXT )MiniportAdapterContext;

    UNREFERENCED_PARAMETER(NetBufferLists);
    UNREFERENCED_PARAMETER(ReturnFlags);
}

BOOLEAN
AdapterCheckForHangEx(
    __in  NDIS_HANDLE MiniportAdapterContext
    )
/*++

Routine Description:

    The MiniportCheckForHangEx handler is called to report the state of the
    NIC, or to monitor the responsiveness of an underlying device driver.
    This is an optional function. If this handler is not specified, NDIS
    judges the driver unresponsive when the driver holds
    MiniportQueryInformation or MiniportSetInformation requests for a
    time-out interval (deafult 4 sec), and then calls the driver's
    MiniportReset function. A NIC driver's MiniportInitialize function can
    extend NDIS's time-out interval by calling NdisMSetAttributesEx to
    avoid unnecessary resets.

    MiniportCheckForHangEx runs at IRQL <= DISPATCH_LEVEL.

Arguments:

    MiniportAdapterContext  Pointer to our adapter

Return Value:

    TRUE    NDIS calls the driver's MiniportReset function.
    FALSE   Everything is fine

--*/
{
    PTAP_ADAPTER_CONTEXT   adapter = (PTAP_ADAPTER_CONTEXT )MiniportAdapterContext;

    DEBUGP (("[TAP] --> AdapterCheckForHangEx\n"));

    DEBUGP (("[TAP] <-- AdapterCheckForHangEx; status = FALSE\n"));

    return FALSE;   // Everything is fine
}

NDIS_STATUS
AdapterReset(
    __in   NDIS_HANDLE            MiniportAdapterContext,
    __out PBOOLEAN                AddressingReset
    )
{
    PTAP_ADAPTER_CONTEXT   adapter = (PTAP_ADAPTER_CONTEXT )MiniportAdapterContext;
    NDIS_STATUS    status;

    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(AddressingReset);

    status = NDIS_STATUS_HARD_ERRORS;

    return status;
}

VOID
AdapterDevicePnpEventNotify(
    __in  NDIS_HANDLE             MiniportAdapterContext,
    __in  PNET_DEVICE_PNP_EVENT   NetDevicePnPEvent
    )
{
    PTAP_ADAPTER_CONTEXT   adapter = (PTAP_ADAPTER_CONTEXT )MiniportAdapterContext;

/*
    switch (NetDevicePnPEvent->DevicePnPEvent)
    {
        case NdisDevicePnPEventSurpriseRemoved:
            //
            // Called when NDIS receives IRP_MN_SUPRISE_REMOVAL.
            // NDIS calls MiniportHalt function after this call returns.
            //
            MP_SET_FLAG(Adapter, fMP_ADAPTER_SURPRISE_REMOVED);
            DEBUGP(MP_INFO, "[%p] MPDevicePnpEventNotify: NdisDevicePnPEventSurpriseRemoved\n", Adapter);
            break;

        case NdisDevicePnPEventPowerProfileChanged:
            //
            // After initializing a miniport driver and after miniport driver
            // receives an OID_PNP_SET_POWER notification that specifies
            // a device power state of NdisDeviceStateD0 (the powered-on state),
            // NDIS calls the miniport's MiniportPnPEventNotify function with
            // PnPEvent set to NdisDevicePnPEventPowerProfileChanged.
            //
            DEBUGP(MP_INFO, "[%p] MPDevicePnpEventNotify: NdisDevicePnPEventPowerProfileChanged\n", Adapter);

            if (NetDevicePnPEvent->InformationBufferLength == sizeof(ULONG))
            {
                ULONG NdisPowerProfile = *((PULONG)NetDevicePnPEvent->InformationBuffer);

                if (NdisPowerProfile == NdisPowerProfileBattery)
                {
                    DEBUGP(MP_INFO, "[%p] The host system is running on battery power\n", Adapter);
                }
                if (NdisPowerProfile == NdisPowerProfileAcOnLine)
                {
                    DEBUGP(MP_INFO, "[%p] The host system is running on AC power\n", Adapter);
                }
            }
            break;

        default:
            DEBUGP(MP_ERROR, "[%p] MPDevicePnpEventNotify: unknown PnP event 0x%x\n", Adapter, NetDevicePnPEvent->DevicePnPEvent);
    }
*/
}

VOID
AdapterShutdownEx(
    __in  NDIS_HANDLE             MiniportAdapterContext,
    __in  NDIS_SHUTDOWN_ACTION    ShutdownAction
    )
/*++

Routine Description:

    The MiniportShutdownEx handler restores hardware to its initial state when
    the system is shut down, whether by the user or because an unrecoverable
    system error occurred. This is to ensure that the NIC is in a known
    state and ready to be reinitialized when the machine is rebooted after
    a system shutdown occurs for any reason, including a crash dump.

    Here just disable the interrupt and stop the DMA engine.  Do not free
    memory resources or wait for any packet transfers to complete.  Do not call
    into NDIS at this time.

    This can be called at aribitrary IRQL, including in the context of a
    bugcheck.

Arguments:

    MiniportAdapterContext  Pointer to our adapter
    ShutdownAction  The reason why NDIS called the shutdown function

Return Value:

    None.

--*/
{
    PTAP_ADAPTER_CONTEXT   adapter = (PTAP_ADAPTER_CONTEXT )MiniportAdapterContext;

    UNREFERENCED_PARAMETER(ShutdownAction);
    UNREFERENCED_PARAMETER(MiniportAdapterContext);

    //
    // We don't have any hardware to reset.
    //
}


// Free adapter context memory and associated resources.
VOID
tapAdapterContextFree(
    IN PTAP_ADAPTER_CONTEXT     Adapter
    )
{
    PLIST_ENTRY listEntry = &Adapter->AdapterListLink;

    // Adapter contrxt should already be removed.
    ASSERT( (listEntry->Flink == listEntry) && (listEntry->Blink == listEntry ) );

    // Insure that adapter context has been removed from global adapter list.
    RemoveEntryList(&Adapter->AdapterListLink);

    // Free the ANSI NetCfgInstanceId buffer.
    if(Adapter->NetCfgInstanceIdAnsi.Buffer != NULL)
    {
        RtlFreeAnsiString(&Adapter->NetCfgInstanceIdAnsi);
    }

    Adapter->NetCfgInstanceIdAnsi.Buffer = NULL;

    NdisFreeMemory(Adapter,0,0);
}
