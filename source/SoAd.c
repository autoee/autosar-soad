/* Copyright (C) 2015 Joakim Plate
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * @file
 * @ingroup SoAd
 */

/**
 * @addgroup SoAd SoAd
 *
 * An implementation of the AUTOSAR SoAd module
 */

#include "Std_Types.h"
#include "ComStack_Types.h"
#include "SoAd.h"
#include "PduR_SoAd.h"
#include <string.h>

#if(SOAD_CFG_ENABLE_DEVELOPMENT_ERROR == STD_ON)
#include "Det.h"
#define SOAD_DET_ERROR(api, error) Det_ReportError(SOAD_MODULEID, SOAD_INSTANCEID, api, error)
#define SOAD_DET_CHECK_RET(check, api, error)        \
    do {                                             \
        if (!(check)) {                              \
            (void)Det_ReportError(SOAD_MODULEID      \
                                , SOAD_INSTANCEID    \
                                , api                \
                                , error);            \
            return E_NOT_OK;                         \
        }                                            \
    } while(0)

#define SOAD_DET_CHECK_RET_0(check, api, error)      \
    do {                                             \
        if (!(check)) {                              \
            (void)Det_ReportError(SOAD_MODULEID      \
                                , SOAD_INSTANCEID    \
                                , api                \
                                , error);            \
            return;                                  \
        }                                            \
    } while(0)

#else
#define SOAD_DET_ERROR(api, error)
#define SOAD_DET_CHECK_RET(check, api, error)
#define SOAD_DET_CHECK_RET_0(check, api)
#endif

const SoAd_ConfigType * SoAd_Config = NULL_PTR;

typedef struct {
    TcpIp_SocketIdType        socket_id;
    TcpIp_SockAddrStorageType remote;
    SoAd_SoConStateType       state;
    boolean                   request_open;
    boolean                   request_close;
    boolean                   request_abort;

    const SoAd_SocketRouteType* rx_route;
    const SoAd_PduRouteType*    tx_route;
    PduLengthType               tx_remain;
    PduLengthType               tx_available;

} SoAd_SoConStatusType;

typedef struct {
    TcpIp_SocketIdType        socket_id;
} SoAd_SoGrpStatusType;

SoAd_SoConStatusType       SoAd_SoConStatus[SOAD_CFG_CONNECTION_COUNT];
SoAd_SoGrpStatusType       SoAd_SoGrpStatus[SOAD_CFG_CONNECTIONGROUP_COUNT];

static const uint32 SoAd_Ip6Any[] = {
        TCPIP_IP6ADDR_ANY,
        TCPIP_IP6ADDR_ANY,
        TCPIP_IP6ADDR_ANY,
        TCPIP_IP6ADDR_ANY
};

static void SoAd_SockAddrCopy(TcpIp_SockAddrStorageType* trg, const TcpIp_SockAddrType* src)
{
    switch (src->domain) {
        case TCPIP_AF_INET:
            trg->inet = *(const TcpIp_SockAddrInetType*)src;
            break;
        case TCPIP_AF_INET6:
            trg->inet6 = *(const TcpIp_SockAddrInet6Type*)src;
            break;
        default:
            break;
    }
}

/**
 * @brief Check if a socket address contains any wildcards
 * @param[in] addr Socket address to check
 * @return TRUE if socket address contain any wildcards
 */
static boolean SoAd_SockAddrWildcard(const TcpIp_SockAddrType* addr)
{
    boolean res = FALSE;
    switch (addr->domain) {
        case TCPIP_AF_INET: {
                const TcpIp_SockAddrInetType* inet = (const TcpIp_SockAddrInetType*)addr;
                if (inet->addr[0] == TCPIP_IPADDR_ANY) {
                    res = TRUE;
                }

                if (inet->port == TCPIP_PORT_ANY) {
                    res = TRUE;
                }
            }
            break;
        case TCPIP_AF_INET6: {
                const TcpIp_SockAddrInet6Type* inet6 = (const TcpIp_SockAddrInet6Type*)addr;
                if (memcmp(inet6->addr, SoAd_Ip6Any, sizeof(SoAd_Ip6Any)) == 0) {
                    res = TRUE;
                }

                if (inet6->port == TCPIP_PORT_ANY) {
                    res = TRUE;
                }
            }
            break;
        default:
            break;
    }
    return res;
}


/**
 * @brief Check if a socket address contains any wildcards
 * @param[in] addr Socket address to check
 * @return TRUE if socket address contain any wildcards
 */
static boolean SoAd_SockAddrWildcardMatch(const TcpIp_SockAddrType* addr_mask, const TcpIp_SockAddrType* addr_check)
{
    boolean res = FALSE;
    if (addr_mask->domain == addr_check->domain) {
        switch (addr_mask->domain) {
            case TCPIP_AF_INET: {
                    const TcpIp_SockAddrInetType* inet_mask  = (const TcpIp_SockAddrInetType*)addr_mask;
                    const TcpIp_SockAddrInetType* inet_check = (const TcpIp_SockAddrInetType*)addr_check;

                    if ((inet_mask->addr[0] == TCPIP_IPADDR_ANY)
                    ||  (inet_mask->addr[0] == inet_check->addr[0])) {
                        if ((inet_mask->port    == TCPIP_PORT_ANY)
                        ||  (inet_mask->port    == inet_check->port)) {
                            res = TRUE;
                        }
                    }
                }
                break;
            case TCPIP_AF_INET6: {
                    const TcpIp_SockAddrInet6Type* inet_mask  = (const TcpIp_SockAddrInet6Type*)addr_mask;
                    const TcpIp_SockAddrInet6Type* inet_check = (const TcpIp_SockAddrInet6Type*)addr_check;

                    if ((memcmp(SoAd_Ip6Any    , inet_check->addr, sizeof(inet_check->addr)) == sizeof(inet_check->addr))
                    ||  (memcmp(inet_mask->addr, inet_check->addr, sizeof(inet_check->addr)) == sizeof(inet_check->addr))) {
                        if ((inet_mask->port    == TCPIP_PORT_ANY)
                        ||  (inet_mask->port    == inet_check->port)) {
                            res = TRUE;
                        }
                    }
                }
                break;
            default:
                res = E_NOT_OK;
                break;
        }
    } else {
        res = E_NOT_OK;
    }
    return res;
}


static void SoAd_Init_SoCon(SoAd_SoConIdType id)
{
    const SoAd_SoConConfigType* config = SoAd_Config->connections[id];
    SoAd_SoConStatusType*       status = &SoAd_SoConStatus[id];

    memset(status, 0, sizeof(*status));
    if (config->remote) {
        SoAd_SockAddrCopy(&status->remote, config->remote);
    } else {
        status->remote.base.domain = (TcpIp_DomainType)0u;
    }
    status->socket_id = TCPIP_SOCKETID_INVALID;

    /** @req SWS_SoAd_00723 */
    status->state     = SOAD_SOCON_OFFLINE;
}

static void SoAd_Init_SoGrp(SoAd_SoGrpIdType id)
{
    const SoAd_SoGrpConfigType* config = SoAd_Config->groups[id];
    SoAd_SoGrpStatusType*       status = &SoAd_SoGrpStatus[id];
    memset(status, 0, sizeof(*status));
    status->socket_id = TCPIP_SOCKETID_INVALID;
}

static Std_ReturnType SoAd_SoCon_Lookup(SoAd_SoConIdType *id, TcpIp_SocketIdType socket_id)
{
    Std_ReturnType   res = E_NOT_OK;
    SoAd_SoConIdType index;
    for (index = 0u; index < SOAD_CFG_CONNECTION_COUNT; ++index) {
        if (SoAd_SoConStatus[index].socket_id == socket_id) {
            res = E_OK;
            *id = index;
            break;
        }
    }
    return res;
}

static Std_ReturnType SoAd_SoGrp_Lookup(SoAd_SoGrpIdType *id, TcpIp_SocketIdType socket_id)
{
    Std_ReturnType   res = E_NOT_OK;
    SoAd_SoConIdType index;
    for (index = 0u; index < SOAD_CFG_CONNECTIONGROUP_COUNT; ++index) {
        if (SoAd_SoGrpStatus[index].socket_id == socket_id) {
            res = E_OK;
            *id = index;
            break;
        }
    }
    return res;
}

static Std_ReturnType SoAd_SoCon_Lookup_FreeSocket(
        SoAd_SoConIdType*         id,
        SoAd_SoGrpIdType          group,
        const TcpIp_SockAddrType* remote
    )
{
    Std_ReturnType   res = E_NOT_OK;
    SoAd_SoConIdType index;
    for (index = 0u; index < SOAD_CFG_CONNECTION_COUNT; ++index) {
        const SoAd_SoConConfigType* config = SoAd_Config->connections[index];
        const SoAd_SoConStatusType* status = &SoAd_SoConStatus[index];

        if (status->socket_id != TCPIP_SOCKETID_INVALID) {
            continue;
        }

        if (status->state != SOAD_SOCON_OFFLINE) {
            if (config->group == group) {
                if (SoAd_SockAddrWildcardMatch((TcpIp_SockAddrType*)&status->remote, remote) == TRUE) {
                    res = E_OK;
                    *id = index;
                    break;
                }
            }
        }
    }
    return res;
}

static void SoAd_SoCon_EnterState(SoAd_SoConIdType id, SoAd_SoConStateType);

void SoAd_Init(const SoAd_ConfigType* config)
{
    uint16 id;

    SoAd_Config       = config;


    /** @req SWS_SoAd_00723 */
    for (id = 0u; id < SOAD_CFG_CONNECTION_COUNT; ++id) {
        SoAd_Init_SoCon(id);
    }

    for (id = 0u; id < SOAD_CFG_CONNECTIONGROUP_COUNT; ++id) {
        SoAd_Init_SoGrp(id);
    }
}

static Std_ReturnType SoAd_GetSocketRoute(SoAd_SoConIdType con_id, uint32 header_id, SoAd_SocketRouteIdType* route_id)
{
    Std_ReturnType              res;
    const SoAd_SoConConfigType* con_config = SoAd_Config->connections[con_id];
    const SoAd_SoGrpConfigType* grp_config = SoAd_Config->groups[con_config->group];

    /* TODO - there can be multiple routes mapped to each connection */
    if (con_config->socket_route_id != SOAD_SOCKETROUTEID_INVALID) {
        *route_id = con_config->socket_route_id;
        res = E_OK;
    } else if (grp_config->socket_route_id < SOAD_SOCKETROUTEID_INVALID) {
        *route_id = grp_config->socket_route_id;
        res = E_OK;
    } else {
        res = E_NOT_OK;
    }
    return res;
}

static Std_ReturnType SoAd_GetPduRoute(PduIdType id, const SoAd_PduRouteType** route)
{
    Std_ReturnType res;
    PduIdType      high = SOAD_CFG_PDUROUTE_COUNT - 1u;
    PduIdType      low  = 0u;

    while (low < high) {
        PduIdType mid = low + (PduIdType)((high - low) >> 1u);
        if (SoAd_Config->pdu_routes[mid]->pdu_id < id) {
            low = mid + 1u;
        } else {
            low = mid;
        }
    }

    if (SoAd_Config->pdu_routes[low]->pdu_id == id) {
        *route = SoAd_Config->pdu_routes[low];
        res = E_OK;
    } else {
        res = E_NOT_OK;
    }
    return res;
}

/**
 * @brief Performs check to see if socket should go online
 * @req   SWS_SoAd_00592
 */
static void SoAd_RxIndication_RemoteOnline(SoAd_SoConIdType con_id, const TcpIp_SockAddrType* remote, TcpIp_SockAddrStorageType* restore, SoAd_SoConStateType* state)
{
    SoAd_SoConStatusType*       con_status = &SoAd_SoConStatus[con_id];

    *state = con_status->state;
    if (con_status->state != SOAD_SOCON_ONLINE) {
        const SoAd_SoConConfigType* con_config = SoAd_Config->connections[con_id];
        const SoAd_SoGrpConfigType* grp_config = SoAd_Config->groups[con_config->group];
        if (grp_config->protocol == TCPIP_IPPROTO_UDP) {
            if (grp_config->listen_only == FALSE) {
                if (SoAd_SockAddrWildcard(&con_status->remote.base) == TRUE) {
                    /* TODO - (4) SoAdSocketMsgAcceptanceFilterEnabled */
                    /* TODO - (6) Acceptance policy */
                    *restore = con_status->remote;
                    SoAd_SockAddrCopy(&con_status->remote, remote);
                    SoAd_SoCon_EnterState(con_id, SOAD_SOCON_ONLINE);
                }
            }
        }
    }
}

/**
 * @brief Revert remote address change if state mismatches
 * @req SWS_SoAd_00710
 */
static void SoAd_RxIndication_RemoteRevert(SoAd_SoConIdType con_id, const TcpIp_SockAddrStorageType* remote, const SoAd_SoConStateType state)
{
    SoAd_SoConStatusType* con_status = &SoAd_SoConStatus[con_id];

    if (con_status->state != state) {
        con_status->remote = *remote;
        SoAd_SoCon_EnterState(con_id, state);
    }
}

Std_ReturnType SoAd_RxIndication_SoCon(
        SoAd_SoConIdType            con_id,
        uint8*                      buf,
        uint16                      len
    )
{
    PduInfoType                 info;
    const SoAd_SoConStatusType* con_sts = &SoAd_SoConStatus[con_id];

    /* TODO - header id handling */


    if (con_sts->rx_route) {
        PduLengthType     buf_len;

        info.SduDataPtr = NULL_PTR;
        info.SduLength  = 0u;

        if (con_sts->rx_route->destination.upper->copy_rx_data(
                con_sts->rx_route->destination.pdu
              , &info
              , &buf_len) != BUFREQ_OK) {
            return E_NOT_OK;
        }

        /* TODO check for internal buffer */
        if (buf_len < len) {
            return E_NOT_OK;
        }


        info.SduLength = len;
        info.SduDataPtr = buf;

        if (con_sts->rx_route->destination.upper->copy_rx_data(
                con_sts->rx_route->destination.pdu
              , &info
              , &buf_len) != BUFREQ_OK) {
            return E_NOT_OK;
        }

    }

    return E_OK;
}

void SoAd_RxIndication(
        TcpIp_SocketIdType          socket_id,
        const TcpIp_SockAddrType*   remote,
        uint8*                      buf,
        uint16                      len
    )
{
    SoAd_SoConIdType id_con;
    Std_ReturnType   res;

    /**
     * @req SWS_SoAd_00264
     */
    SOAD_DET_CHECK_RET_0(SoAd_Config != NULL_PTR
                       , SOAD_API_RXINDICATION
                       , SOAD_E_NOTINIT);

    /**
     * @req SWS_SoAd_00264
     */
    SOAD_DET_CHECK_RET_0(remote != NULL_PTR
                       , SOAD_API_RXINDICATION
                       , SOAD_E_INV_ARG);


    res = SoAd_SoCon_Lookup(&id_con, socket_id);
    if (res != E_OK) {
        SoAd_SoGrpIdType id_grp;
        res = SoAd_SoGrp_Lookup(&id_grp, socket_id);
        if (res == E_OK) {
            res = SoAd_SoCon_Lookup_FreeSocket(&id_con, id_grp, remote);
        }
    }

    if (res == E_OK) {
        TcpIp_SockAddrStorageType   revert_remote;
        SoAd_SoConStateType         revert_state;
        SoAd_RxIndication_RemoteOnline(id_con, remote, &revert_remote, &revert_state);

        res = SoAd_RxIndication_SoCon(id_con, buf, len);

        if (res != E_OK) {
            SoAd_RxIndication_RemoteRevert(id_con, &revert_remote, revert_state);
        }
    } else {
        /**
         * @req SWS_SoAd_00267
         */
        SOAD_DET_ERROR(SOAD_API_RXINDICATION
                     , SOAD_E_INV_SOCKETID);
    }
}

/**
 * @brief Close down a socket group
 *
 * @req SWS_SoAd_00646
 * @req SWS_SoAd_00643
 *
 * Specification is unclear on how to handle sockets
 * that are in RECONNECT state, ie for example all
 * child sockets of a listening group socket. This
 * implementation will close down all group sockets
 * if the "master" socket is lost.
 */
static void SoAd_SoGrp_Close(SoAd_SoGrpIdType id_grp)
{
    SoAd_SoConIdType id_con;

    const SoAd_SoGrpConfigType* config_grp = SoAd_Config->groups[id_con];
    SoAd_SoGrpStatusType* status_grp = &SoAd_SoGrpStatus[id_con];
    status_grp->socket_id = TCPIP_SOCKETID_INVALID;

    for (id_con = 0u; id_con < SOAD_CFG_CONNECTION_COUNT; ++id_con) {
        const SoAd_SoConConfigType* config = SoAd_Config->connections[id_con];
        const SoAd_SoConStatusType* status = &SoAd_SoConStatus[id_con];
        if (config->group == id_grp && status->socket_id == TCPIP_SOCKETID_INVALID) {
            SoAd_SoCon_EnterState(id_con, SOAD_SOCON_OFFLINE);
        }
    }
}

void SoAd_TcpIpEvent(
        TcpIp_SocketIdType          socket_id,
        TcpIp_EventType             event
    )
{
    SoAd_SoConIdType id_con;
    SoAd_SoGrpIdType id_grp;
    Std_ReturnType   res;

    /**
     * @req SWS_SoAd_00276
     */
    SOAD_DET_CHECK_RET_0(SoAd_Config != NULL_PTR
                       , SOAD_API_TCPIPEVENT
                       , SOAD_E_NOTINIT);


    switch (event) {
        case TCPIP_TCP_FIN_RECEIVED:
            (void)TcpIp_Close(socket_id, FALSE);
            break;

        case TCPIP_TCP_RESET:
        case TCPIP_TCP_CLOSED:
        case TCPIP_UDP_CLOSED:
            res = SoAd_SoGrp_Lookup(&id_grp, socket_id);
            if (res == E_OK) {
                SoAd_SoGrp_Close(id_grp);
            } else {
                res = SoAd_SoCon_Lookup(&id_con, socket_id);
                if (res == E_OK) {
                    SoAd_SoCon_EnterState(id_con, SOAD_SOCON_OFFLINE);
                } else {
                    /**
                     * @req SWS_SoAd_00277
                     */
                    SOAD_DET_ERROR(SOAD_API_TCPIPEVENT
                                 , SOAD_E_INV_SOCKETID);
                }
            }
            break;
        default:
            SOAD_DET_ERROR(SOAD_API_TCPIPEVENT
                         , SOAD_E_INV_ARG);
            break;
    }

}

void SoAd_TxConfirmation(
        TcpIp_SocketIdType          socket_id,
        uint16                      len
    )
{
    SoAd_SoConIdType id;
    Std_ReturnType   res;

    res = SoAd_SoCon_Lookup(&id, socket_id);
    if (res == E_OK) {

    }
}

Std_ReturnType SoAd_TcpAccepted(
        TcpIp_SocketIdType          socket_id,
        TcpIp_SocketIdType          socket_id_connected,
        const TcpIp_SockAddrType*   remote
    )
{
    SoAd_SoGrpIdType id_group;
    Std_ReturnType   res;

    res = SoAd_SoGrp_Lookup(&id_group, socket_id);
    if (res == E_OK) {
        const SoAd_SoGrpConfigType* group  = SoAd_Config->groups[id_group];
        SoAd_SoConIdType            id_connected;

        if (group->initiate == FALSE) {
            res = SoAd_SoCon_Lookup_FreeSocket(&id_connected, id_group, remote);
            if (res == E_OK) {
                SoAd_SoConStatusType* status_connected = &SoAd_SoConStatus[id_connected];
                status_connected->socket_id = socket_id_connected;
                SoAd_SockAddrCopy(&status_connected->remote, remote);
                SoAd_SoCon_EnterState(id_connected, SOAD_SOCON_ONLINE);
            }
        }
    }

    return res;
}

void SoAd_TcpConnected(
        TcpIp_SocketIdType          socket_id
    )
{
    SoAd_SoConIdType id;
    Std_ReturnType   res;

    res = SoAd_SoCon_Lookup(&id, socket_id);
    if (res == E_OK) {
        const SoAd_SoConConfigType* config = SoAd_Config->connections[id];
        const SoAd_SoGrpConfigType* group  = SoAd_Config->groups[config->group];
        SoAd_SoConStatusType*       status = &SoAd_SoConStatus[id];

        if (group->initiate) {
            if (status->state != SOAD_SOCON_ONLINE) {
                if (group->protocol == TCPIP_IPPROTO_TCP) {
                    SoAd_SoCon_EnterState(id, SOAD_SOCON_ONLINE);
                }
            }
        }
    }
}

BufReq_ReturnType SoAd_CopyTxData(
        TcpIp_SocketIdType          socket_id,
        uint8*                      buf,
        uint16                      len
    )
{
    Std_ReturnType       res;
    BufReq_ReturnType    res_buf;
    SoAd_SoConIdType     id_con;
    res = SoAd_SoCon_Lookup(&id_con, socket_id);
    if (res == E_OK) {
        const SoAd_SoConConfigType* config = SoAd_Config->connections[id_con];
        SoAd_SoConStatusType*       status = &SoAd_SoConStatus[id_con];
        PduInfoType                 info;

        info.SduLength  = len;
        info.SduDataPtr = buf;

        res_buf = status->tx_route->upper->copy_tx_data(status->tx_route->pdu_id
                                                       , &info
                                                       , NULL_PTR
                                                       , &status->tx_available);
        if (res_buf == BUFREQ_OK) {
            status->tx_remain -= len;
        }
    } else {
        res_buf = BUFREQ_E_NOT_OK;
    }

    return res_buf;
}

Std_ReturnType SoAd_IfTransmit(
        PduIdType                   pdu_id,
        const PduInfoType*          pdu_info
    )
{
    Std_ReturnType              res;
    const SoAd_PduRouteType*    route;

    /**
     * @req SWS_SoAd_00213
     */
    SOAD_DET_CHECK_RET(SoAd_Config != NULL_PTR
                     , SOAD_API_IFTRANSMIT
                     , SOAD_E_NOTINIT);

    /**
     * @req SWS_SoAd_00214
     */
    SOAD_DET_CHECK_RET(pdu_id < SOAD_CFG_PDUROUTE_COUNT
                     , SOAD_API_IFTRANSMIT
                     , SOAD_E_INV_PDUID);

    /**
     * @req SWS_SoAd_00653-TODO
     */

    res = SoAd_GetPduRoute(pdu_id, &route);

    if (res == E_OK) {
        SoAd_SoConStatusType*       status;
        const SoAd_SoConConfigType* config;
        const SoAd_SoGrpConfigType* group;
        Std_ReturnType              res;

        status = &SoAd_SoConStatus[route->destination.connection];
        config = SoAd_Config->connections[route->destination.connection];
        group  = SoAd_Config->groups[config->group];

        if (status->state == SOAD_SOCON_ONLINE) {
            switch(group->protocol) {
                case TCPIP_IPPROTO_UDP:
                    res = TcpIp_UdpTransmit(status->socket_id
                                          , pdu_info->SduDataPtr
                                          , &status->remote.base
                                          , pdu_info->SduLength);
                    break;
                case TCPIP_IPPROTO_TCP:
                    res = TcpIp_TcpTransmit(status->socket_id
                                          , pdu_info->SduDataPtr
                                          , pdu_info->SduLength
                                          , TRUE);
                    break;
                default:
                    res = E_NOT_OK;
                    break;
            }
        } else {
            res = E_NOT_OK;
        }
    }
    return res;
}


Std_ReturnType SoAd_TpTransmit(
        PduIdType                   pdu_id,
        const PduInfoType*          pdu_info
    )
{
    Std_ReturnType              res;
    const SoAd_PduRouteType*    route;

    /**
     * @req SWS_SoAd_00224
     */
    SOAD_DET_CHECK_RET(SoAd_Config != NULL_PTR
                     , SOAD_API_TPTRANSMIT
                     , SOAD_E_NOTINIT);

    /**
     * @req SWS_SoAd_00237
     */
    SOAD_DET_CHECK_RET(pdu_id < SOAD_CFG_PDUROUTE_COUNT
                     , SOAD_API_TPTRANSMIT
                     , SOAD_E_INV_PDUID);

    /**
     * @req SWS_SoAd_00650-TODO
     */

    res = SoAd_GetPduRoute(pdu_id, &route);

    if (res == E_OK) {
        SoAd_SoConStatusType* status;
        status = &SoAd_SoConStatus[route->destination.connection];
        status->tx_route = route;
    }
    return res;
}

/**
 * @brief SWS_SoAd_00642-TODO
 */
void SoAd_SoCon_ProcessClose(SoAd_SoConIdType id)
{
    const SoAd_SoConConfigType* config = SoAd_Config->connections[id];
    SoAd_SoConStatusType*       status       = &SoAd_SoConStatus[id];

    if (status->request_close) {
        if (status->socket_id != TCPIP_SOCKETID_INVALID) {
            TcpIp_Close(status->socket_id, status->request_abort);
        }
        status->request_close = FALSE;
    }
}

void SoAd_SoCon_ProcessTransmit(SoAd_SoConIdType id)
{
    SoAd_SoConStatusType*       status;
    const SoAd_SoConConfigType* config;
    const SoAd_SoGrpConfigType* group;
    const SoAd_PduRouteType*    route;
    Std_ReturnType              res;
    BufReq_ReturnType           res_buf;
    PduInfoType                 pdu_info;

    status = &SoAd_SoConStatus[id];
    config = SoAd_Config->connections[id];
    group  = SoAd_Config->groups[config->group];
    route  = status->tx_route;

    if (route) {
        pdu_info.SduDataPtr = NULL_PTR;
        pdu_info.SduLength  = 0u;

        if (status->tx_available == 0u) {
            res_buf = route->upper->copy_tx_data(route->pdu_id, &pdu_info, NULL_PTR, &status->tx_available);
        } else {
            res_buf = BUFREQ_OK;
        }

        if (res_buf == BUFREQ_OK) {
            switch(group->protocol) {
                case TCPIP_IPPROTO_UDP:
                    res = TcpIp_UdpTransmit(status->socket_id
                                          , NULL_PTR
                                          , &status->remote.base
                                          , status->tx_available);
                    break;
                case TCPIP_IPPROTO_TCP:
                    res = TcpIp_TcpTransmit(status->socket_id
                                          , NULL_PTR
                                          , status->tx_available
                                          , FALSE);
                    break;
                default:
                    res = E_NOT_OK;
                    break;
            }
        } else if (res_buf == BUFREQ_E_BUSY) {
            res = E_OK;
        } else {
            res = E_NOT_OK;
        }

        if (status->tx_remain == 0u || (res != E_OK)) {
            /** TODO - SoAdSocketTcpImmediateTpTxConfirmation==FALSE */
            status->tx_route     = NULL_PTR;
            status->tx_remain    = 0u;
            status->tx_available = 0u;
            route->upper->tx_confirmation(route->pdu_id, res);
        }
    }
}

void SoAd_SoCon_State_Online(SoAd_SoConIdType id)
{
    SoAd_SoCon_ProcessClose(id);
    SoAd_SoCon_ProcessTransmit(id);
}

void SoAd_SoCon_State_Reconnect(SoAd_SoConIdType id)
{
    SoAd_SoCon_ProcessClose(id);
}

/**
 * Check if we perform an open on the socket
 * @req  SWS_SoAd_00589
 * @todo TCPIP_IPADDR_STATE_ASSIGNED
 * @todo Only first socket of a tcp group should be opened
 */
static Std_ReturnType SoAd_SoCon_CheckOpen(SoAd_SoConIdType id)
{
    const SoAd_SoConConfigType* config       = SoAd_Config->connections[id];
    const SoAd_SoGrpConfigType* config_group = SoAd_Config->groups[config->group];
    SoAd_SoConStatusType*       status       = &SoAd_SoConStatus[id];
    SoAd_SoGrpStatusType*       status_group = &SoAd_SoGrpStatus[config->group];
    Std_ReturnType              res = E_NOT_OK;

    if (status->socket_id == TCPIP_SOCKETID_INVALID) {
        if ((config_group->automatic != FALSE) || (status->request_open != FALSE)) {
            if (status->remote.base.domain != (TcpIp_DomainType)0u) {
                res = E_OK;
            }
        }
    }

    return res;
}

/**
 * @brief open a socket
 * @req   SWS_SoAd_00590
 * @req   SWS_SoAd_00638
 * @todo  SoAdSocketLocalAddressRef
 * @todo  SWS_SoAd_00689 Socket parameters
 * @todo  MaxChannels of socket group
 */
static Std_ReturnType SoAd_SoCon_PerformOpen(SoAd_SoConIdType id)
{
    const SoAd_SoConConfigType* config       = SoAd_Config->connections[id];
    const SoAd_SoGrpConfigType* config_group = SoAd_Config->groups[config->group];
    SoAd_SoConStatusType*       status       = &SoAd_SoConStatus[id];
    SoAd_SoGrpStatusType*       status_group = &SoAd_SoGrpStatus[config->group];
    Std_ReturnType              res;
    TcpIp_SocketIdType         *socket_id;

    status->request_open = FALSE;

    /*
     * for initiating sockets, the connection itself needs a socket
     * for waiting sockets, it's the socket group that holds the socket
     */
    if (config_group->initiate) {
        socket_id = &status->socket_id;
    } else {
        socket_id = &status_group->socket_id;
    }

    if (*socket_id == TCPIP_SOCKETID_INVALID) {
        res = TcpIp_SoAdGetSocket(config_group->domain
                                , config_group->protocol
                                , socket_id);
        if (res == E_OK) {
            uint16 localport = config_group->localport;
            res = TcpIp_Bind(*socket_id
                           , config_group->localaddr
                           , &localport);

            if (res == E_OK) {
                if (config_group->protocol == TCPIP_IPPROTO_TCP) {
                    if (config_group->initiate) {
                        res = TcpIp_TcpConnect(*socket_id
                                             , &status->remote.base);
                    } else {
                        res = TcpIp_TcpListen(*socket_id
                                             , SOAD_CFG_CONNECTION_COUNT);
                    }
                }
            }

            /* on failure, we must clean up the socket so will try again */
            if (res != E_OK) {
                TcpIp_Close(*socket_id, TRUE);
                *socket_id = TCPIP_SOCKETID_INVALID;
            }
        }
    } else {
        res = E_OK;
    }

    return res;
}

void SoAd_SoCon_State_Offline(SoAd_SoConIdType id)
{
    const SoAd_SoConConfigType* config       = SoAd_Config->connections[id];
    const SoAd_SoGrpConfigType* config_group = SoAd_Config->groups[config->group];
    SoAd_SoConStatusType*       status       = &SoAd_SoConStatus[id];
    Std_ReturnType res;

    res = SoAd_SoCon_CheckOpen(id);
    if (res == E_OK) {
        res = SoAd_SoCon_PerformOpen(id);
        if (res == E_OK) {
            if (config_group->protocol == TCPIP_IPPROTO_TCP) {
                SoAd_SoCon_EnterState(id, SOAD_SOCON_RECONNECT);
            } else if (config_group->protocol == TCPIP_IPPROTO_UDP) {

                /**
                 * @req SWS_SoAd_00686
                 * @req SWS_SoAd_00591
                 *
                 * SoAdSocketUdpListenOnly should possibly be checked here, but
                 * it seems redundant based on the wildcard check
                 */

                if (SoAd_SockAddrWildcard(&status->remote.base) == TRUE) {
                    SoAd_SoCon_EnterState(id, SOAD_SOCON_RECONNECT);
                } else {
                    SoAd_SoCon_EnterState(id, SOAD_SOCON_ONLINE);
                }
            }
        }
    }
}

static void SoAd_SoCon_EnterState(SoAd_SoConIdType id, SoAd_SoConStateType state)
{
    const SoAd_SoConConfigType* con_config = SoAd_Config->connections[id];
    const SoAd_SoGrpConfigType* grp_config = SoAd_Config->groups[con_config->group];
    SoAd_SoConStatusType*       con_status = &SoAd_SoConStatus[id];
    SoAd_SocketRouteIdType      route_id;

    /* update connection state */
    switch(state) {
        case SOAD_SOCON_OFFLINE:
            con_status->socket_id = TCPIP_SOCKETID_INVALID;

            if (con_status->rx_route) {
                con_status->rx_route->destination.upper->rx_indication(
                        con_status->rx_route->destination.pdu
                        , E_OK);
                con_status->rx_route = NULL;
            }

            break;

        case SOAD_SOCON_RECONNECT:
            break;

        case SOAD_SOCON_ONLINE: {

            if (SoAd_GetSocketRoute(id, SOAD_PDUHEADERID_INVALID, &route_id) == E_OK) {
                const SoAd_SocketRouteType* route_config = SoAd_Config->socket_routes[route_id];
                PduLengthType               len  = 0u;
                PduInfoType                 info = {0u};

                if (route_config->destination.upper->start_of_reception(
                                           route_config->destination.pdu
                                         , &info
                                         , len
                                         , &len) == BUFREQ_OK) {
                    con_status->rx_route = route_config;
                }
            }
            break;


            break;
        }
        default:
            break;
    }

    con_status->state = state;
}

void SoAd_SoCon_MainFunction(SoAd_SoConIdType id)
{
    SoAd_SoConStatusType* status = &SoAd_SoConStatus[id];

    switch(status->state) {
        case SOAD_SOCON_OFFLINE:
            SoAd_SoCon_State_Offline(id);
            break;
        case SOAD_SOCON_RECONNECT:
            SoAd_SoCon_State_Reconnect(id);
            break;
        case SOAD_SOCON_ONLINE:
            SoAd_SoCon_State_Online(id);
            break;
        default:
            break;
    }
}

void SoAd_MainFunction(void)
{
    SoAd_SoConIdType id;
    for (id = 0u; id < SOAD_CFG_CONNECTION_COUNT; ++id) {
        SoAd_SoCon_MainFunction(id);
    }
}
