/******************************************************************************
* File Name:   secure_tcp_client.c
*
* Description: This file contains task and functions related to secure TCP
* client operation.
*
*******************************************************************************
* Copyright 2022-2024, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/

/* Header file includes */
#include "cyhal.h"
#include "cybsp.h"
#include "cy_retarget_io.h"

/* Ethernet connection manager header files */
#include "cy_ecm.h"
#include "cy_ecm_error.h"

/* Cypress secure socket header file */
#include "cy_secure_sockets.h"
#include "cy_tls.h"

/* Network connectivity utility header file. */
#include "cy_nw_helper.h"

/* FreeRTOS header file */
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>

/* Standard C header file */
#include <string.h>
#include <inttypes.h>
#include <string.h>

/* TCP server task header file */
#include "secure_tcp_client.h"

/* Network credentials header file */
#include "network_credentials.h"

#include "cy_eth_phy_driver.h"
/******************************************************************************
* Macros
******************************************************************************/

#define LED_ON_CMD                         '1'
#define LED_OFF_CMD                        '0'
#define ACK_LED_ON                         "LED ON ACK"
#define ACK_LED_OFF                        "LED OFF ACK"
#define MSG_INVALID_CMD                    "Invalid command"

/* Maximum number of connection retries to the ethernet network */
#define MAX_ETH_RETRY_COUNT                            (3u)

/* Ethernet interface ID */
#ifdef XMC7100D_F176K4160
#define INTERFACE_ID                        CY_ECM_INTERFACE_ETH0
#else
#define INTERFACE_ID                        CY_ECM_INTERFACE_ETH1
#endif
/******************************************************************************
* Function Prototypes
******************************************************************************/

cy_rslt_t connect_to_secure_tcp_server(cy_socket_sockaddr_t address);
cy_rslt_t create_secure_tcp_client_socket();
cy_rslt_t tcp_client_recv_handler(cy_socket_t socket_handle, void *arg);
cy_rslt_t tcp_disconnection_handler(cy_socket_t socket_handle, void *arg);
void read_uart_input(uint8_t* input_buffer_ptr);
void print_heap_usage(char *msg);

/* Establish ethernet connection to the network. */
static cy_rslt_t connect_to_ethernet(void);

/******************************************************************************
* Global Variables
******************************************************************************/

/* TLS credentials of the TCP client. */
static const char tcp_client_cert[] = keyCLIENT_CERTIFICATE_PEM;
static const char client_private_key[] = keyCLIENT_PRIVATE_KEY_PEM;

/* Root CA certificate for TCP server identity verification. */
static const char tcp_server_ca_cert[] = keySERVER_ROOTCA_PEM;

/* Variable to store the TLS identity (certificate and private key). */
void *tls_identity;

/* TCP client socket handle */
cy_socket_t client_handle;

/* Ethernet connection manager handle */
static cy_ecm_t ecm_handle = NULL;

/* Binary semaphore handle to keep track of secure TCP server connection. */
SemaphoreHandle_t connect_to_server;

/*******************************************************************************
 * Function Name: tcp_secure_client_task
 *******************************************************************************
 * Summary:
 *  Task used to establish a secure connection to a remote TCP server and
 *  control the LED state (ON/OFF) based on the command received from TCP server.
 *
 * Parameters:
 *  void *args : Task parameter defined during task creation (unused)
 *
 * Return:
 *  void
 *
 *******************************************************************************/
void tcp_secure_client_task(void *arg)
{
    cy_rslt_t result;
    uint8_t uart_input[UART_BUFFER_SIZE];

    /* IP address and TCP port number of the TCP server to which the TCP client
     * connects to. */
    cy_socket_sockaddr_t tcp_server_address = {
        .ip_address.ip.v4 = TCP_SERVER_IP_ADDRESS,
        .ip_address.version = CY_SOCKET_IP_VER_V4,
        .port = TCP_SERVER_PORT
    };

    /* Connect to ethernet */
    result = connect_to_ethernet();
    if(result!= CY_RSLT_SUCCESS )
    {
        printf("\n Failed to connect to Ethernet! Error code: 0x%08"PRIx32"\n", (uint32_t)result);
        CY_ASSERT(0);
    }

    /* Create a binary semaphore to keep track of secure TCP server connection. */
    connect_to_server = xSemaphoreCreateBinary();

    /* Give the semaphore so as to connect to TCP server. */
    xSemaphoreGive(connect_to_server); 

    /* TCP client certificate length and private key length. */
    const size_t tcp_client_cert_len = strlen( tcp_client_cert );
    const size_t pkey_len = strlen( client_private_key );

    /* Initialize secure socket library. */
    result = cy_socket_init();
    if (result != CY_RSLT_SUCCESS)
    {
        printf("Secure Socket initialization failed!\n");
        CY_ASSERT(0);
    }
    else
    {
        printf("Secure Socket initialized.\n");
    }

    /* Initializes the global trusted RootCA certificate. This examples uses a self signed
     * certificate which implies that the RootCA certificate is same as the certificate of
     * TCP secure server to which client is connecting to.
     */
    result = cy_tls_load_global_root_ca_certificates(tcp_server_ca_cert, strlen(tcp_server_ca_cert));
    if( result != CY_RSLT_SUCCESS)
    {
        printf("cy_tls_load_global_root_ca_certificates failed! Error code: %"PRIu32"\n", result);
    }
    else
    {
        printf("Global trusted RootCA certificate loaded.\n");
    }

    /* Create TCP client identity using the SSL certificate and private key. */
    result = cy_tls_create_identity(tcp_client_cert, tcp_client_cert_len,
                                    client_private_key, pkey_len, &tls_identity);
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Failed cy_tls_create_identity! Error code: %"PRIu32"\n", result);
        CY_ASSERT(0);
    }   

    for(;;)
    {
        /* Wait till semaphore is acquired so as to connect to a secure TCP server. */
        xSemaphoreTake(connect_to_server, portMAX_DELAY);

        printf("Connect to TCP server\r\n");
        printf("Enter the IP address of the TCP Server:\n");

        /* Prevent system from entering deep sleep mode
         * when receiving data from UART.
         */
        cyhal_syspm_lock_deepsleep();

        /* Clear the UART input buffer. */
        memset(uart_input, 0, UART_BUFFER_SIZE);

        /* Read the TCP server's IPv4 address from  the user via the
         * UART terminal.
         */
        read_uart_input(uart_input);

        /* Allow system to enter deep sleep mode. */
        cyhal_syspm_unlock_deepsleep();
        
        cy_nw_aton((char *)uart_input, (cy_nw_ip_address_t *)&tcp_server_address.ip_address);

        cy_nw_ntoa((cy_nw_ip_address_t *)&tcp_server_address.ip_address, (char *)uart_input); 

        printf("\r\nConnecting to TCP Server (IP Address: %s, Port: %d)\n\n", (char *)uart_input , TCP_SERVER_PORT);

        /* Connect to the secure TCP server. If the connection fails, retry
         * to connect to the server for MAX_TCP_SERVER_CONN_RETRIES times. */
        printf("Connecting to TCP server...\n");
        result = connect_to_secure_tcp_server(tcp_server_address);

        if(result != CY_RSLT_SUCCESS)
        {
            printf("Failed to connect to TCP server. Error code: %"PRIu32"\n", result);
            /* Give the semaphore so as to connect to TCP server. */
            xSemaphoreGive(connect_to_server);
        }
        
        print_heap_usage("After connecting to TCP server\n");
    }
 }

cy_ecm_phy_callbacks_t phy_callbacks =
{
        .phy_init = cy_eth_phy_init,
        .phy_configure = cy_eth_phy_configure,
        .phy_enable_ext_reg = cy_eth_phy_enable_ext_reg,
        .phy_discover = cy_eth_phy_discover,
        .phy_get_auto_neg_status = cy_eth_phy_get_auto_neg_status,
        .phy_get_link_partner_cap = cy_eth_phy_get_link_partner_cap,
        .phy_get_linkspeed = cy_eth_phy_get_linkspeed,
        .phy_get_linkstatus = cy_eth_phy_get_linkstatus,
        .phy_reset = cy_eth_phy_reset
};

/*******************************************************************************
 * Function Name: connect_to_ethernet
 *******************************************************************************
 * Summary:
 *  Connects to ethernet, retries up to a
 *  configured number of times until the connection succeeds.
 *
 * Parameters:
 * None
 *
 * Return:
 *  cy_result result: Result of the operation.
 *
 *******************************************************************************/
cy_rslt_t connect_to_ethernet(void)
{
    cy_rslt_t result = CY_RSLT_SUCCESS;

    uint8_t retry_count = 0;

    /* Variables used by ethernet connection manager.*/
    cy_ecm_ip_address_t ip_addr;

    /* Initialize ethernet connection manager. */
    result = cy_ecm_init();
    if (result != CY_RSLT_SUCCESS)
    {
        printf("Ethernet connection manager initialization failed! Error code: 0x%08"PRIx32"\n", (uint32_t)result);
        CY_ASSERT(0);
    }
    else
    {
        printf("Ethernet connection manager initialized.\n");
    }

    /* Initialize the Ethernet Interface and PHY driver */
    result =  cy_ecm_ethif_init(INTERFACE_ID, &phy_callbacks, &ecm_handle);
    if (result != CY_RSLT_SUCCESS)
    {
        printf("Ethernet interface initialization failed! Error code: 0x%08"PRIx32"\n", (uint32_t)result);
        CY_ASSERT(0);
    }

    /* Establish a connection to the ethernet network */
    while(1)
    {
        result = cy_ecm_connect(ecm_handle, NULL, &ip_addr);
        if(result != CY_RSLT_SUCCESS)
        {
            retry_count++;
            if (retry_count >= MAX_ETH_RETRY_COUNT)
            {
                printf("Exceeded max ethernet connection attempts\n");
                return result;
            }
            printf("Connection to ethernet network failed. Retrying...\n");
            continue;
        }
        else
        {
            printf("Successfully connected to ethernet network.\n");
            printf("IP Address Assigned: %d.%d.%d.%d\n", (uint8)ip_addr.ip.v4,(uint8)(ip_addr.ip.v4 >> 8), (uint8)(ip_addr.ip.v4 >> 16),
                                    (uint8)(ip_addr.ip.v4 >> 24));
            break;
        }
    }
    return result;
}

/*******************************************************************************
 * Function Name: create_secure_tcp_client_socket
 *******************************************************************************
 * Summary:
 *  Function to create a secure socket and set the socket options to use TLS
 *  identity, set call back function for handling incoming messages, call back
 *  function to handle disconnection.
 *
 * Return:
 *  cy_result result: Result of the operation.
 *
 *******************************************************************************/
cy_rslt_t create_secure_tcp_client_socket()
{
    cy_rslt_t result;

    /* Variables used to set socket options. */
    cy_socket_opt_callback_t tcp_recv_option;
    cy_socket_opt_callback_t tcp_disconnection_option;

    /* TLS authentication mode. */
    cy_socket_tls_auth_mode_t tls_auth_mode = CY_SOCKET_TLS_VERIFY_REQUIRED;

    /* Create a new secure TCP socket. */
    result = cy_socket_create(CY_SOCKET_DOMAIN_AF_INET, CY_SOCKET_TYPE_STREAM,
                                      CY_SOCKET_IPPROTO_TLS, &client_handle);

    if (result != CY_RSLT_SUCCESS)
    {
        printf("Failed to create socket! Error Code: %"PRIu32"\n", result);
        return result;
    }

    /* Register the callback function to handle messages received from TCP server. */
    tcp_recv_option.callback = tcp_client_recv_handler;
    tcp_recv_option.arg = NULL;
    result = cy_socket_setsockopt(client_handle, CY_SOCKET_SOL_SOCKET,
                                  CY_SOCKET_SO_RECEIVE_CALLBACK,
                                  &tcp_recv_option, sizeof(cy_socket_opt_callback_t));
    if (result != CY_RSLT_SUCCESS)
    {
        printf("Set socket option: CY_SOCKET_SO_RECEIVE_CALLBACK failed! "
                "Error Code: %"PRIu32"\n", result);
        return result;
    }

    /* Register the callback function to handle disconnection. */
    tcp_disconnection_option.callback = tcp_disconnection_handler;
    tcp_disconnection_option.arg = NULL;

    result = cy_socket_setsockopt(client_handle, CY_SOCKET_SOL_SOCKET,
                                  CY_SOCKET_SO_DISCONNECT_CALLBACK,
                                  &tcp_disconnection_option, sizeof(cy_socket_opt_callback_t));
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Set socket option: CY_SOCKET_SO_DISCONNECT_CALLBACK failed! "
                "Error Code: %"PRIu32"\n", result);
        return result;
    }

    /* Set the TCP socket to use the TLS identity. */
    result = cy_socket_setsockopt(client_handle, CY_SOCKET_SOL_TLS, CY_SOCKET_SO_TLS_IDENTITY,
                                  tls_identity, strlen(tls_identity));
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Set socket option: CY_SOCKET_SO_TLS_IDENTITY failed! "
               "Error Code: %"PRIu32"\n", result);
    }

    /* Set the TLS authentication mode. */
    result = cy_socket_setsockopt(client_handle, CY_SOCKET_SOL_TLS, CY_SOCKET_SO_TLS_AUTH_MODE,
                        &tls_auth_mode, sizeof(cy_socket_tls_auth_mode_t));
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Set socket option: CY_SOCKET_SO_TLS_AUTH_MODE failed! "
               "Error Code: %"PRIu32"\n", result);
    }

    return result;
}

/*******************************************************************************
 * Function Name: connect_to_secure_tcp_server
 *******************************************************************************
 * Summary:
 *  Function to connect to secure TCP server.
 *
 * Parameters:
 *  cy_socket_sockaddr_t address: Address of TCP server socket
 *
 * Return:
 *  cy_result result: Result of the operation
 *
 *******************************************************************************/
cy_rslt_t connect_to_secure_tcp_server(cy_socket_sockaddr_t address)
{
    cy_rslt_t conn_result;  

    for(uint32_t conn_retries = 0; conn_retries < MAX_TCP_SERVER_CONN_RETRIES; conn_retries++)
    {
        /* Create a secure TCP socket */
        conn_result = create_secure_tcp_client_socket();
        if(conn_result != CY_RSLT_SUCCESS)
        {
            printf("Failed to create secure socket! Error Code: %"PRIu32"\n", conn_result);
            CY_ASSERT(0);
        }
        
        conn_result = cy_socket_connect(client_handle, &address, sizeof(cy_socket_sockaddr_t));
        if (conn_result == CY_RSLT_SUCCESS)
        {
            printf("============================================================\n");
            printf("TLS Handshake successful and connected to TCP server\n");
            return conn_result;
        }

        printf("Could not connect to TCP server.\n");
        printf("Trying to reconnect to TCP server...Please check if server is listening\n");

        /* The resources allocated during the socket creation (cy_socket_create)
         * should be deleted.
         */
        cy_socket_delete(client_handle);
    }

    /* Stop retrying after maximum retry attempts. */
    printf("Exceeded maximum connection attempts to the TCP server\n");

    return conn_result;
}

/*******************************************************************************
 * Function Name: tcp_client_recv_handler
 *******************************************************************************
 * Summary:
 *  Callback function to handle incoming TCP server messages.
 *
 * Parameters:
 *  cy_socket_t socket_handle: Connection handle for the TCP client socket
 *  void *args : Parameter passed on to the function (unused)
 *
 * Return:
 *  cy_result result: Result of the operation
 *
 *******************************************************************************/
cy_rslt_t tcp_client_recv_handler(cy_socket_t socket_handle, void *arg)
{
    /* Variable to store number of bytes send to the TCP server. */
    uint32_t bytes_sent = 0;

    /* Variable to store number of bytes received. */
    uint32_t bytes_received = 0;

    char message_buffer[MAX_TCP_DATA_PACKET_LENGTH];
    cy_rslt_t result ;

    printf("============================================================\n");
    result = cy_socket_recv(socket_handle, message_buffer, TCP_LED_CMD_LEN,
                            CY_SOCKET_FLAGS_NONE, &bytes_received);

    if(message_buffer[0] == LED_ON_CMD)
    {
        /* Turn the LED ON. */
        cyhal_gpio_write(CYBSP_USER_LED, CYBSP_LED_STATE_ON);
        printf("LED turned ON\n");
        sprintf(message_buffer, ACK_LED_ON);
    }
    else if(message_buffer[0] == LED_OFF_CMD)
    {
        /* Turn the LED OFF. */
        cyhal_gpio_write(CYBSP_USER_LED, CYBSP_LED_STATE_OFF);
        printf("LED turned OFF\n");
        sprintf(message_buffer, ACK_LED_OFF);
    }
    else
    {
        printf("Invalid command\n");
        sprintf(message_buffer, MSG_INVALID_CMD);
    }

    /* Send acknowledgement to the secure TCP server in receipt of the message received. */
    result = cy_socket_send(socket_handle, message_buffer, strlen(message_buffer),
                            CY_SOCKET_FLAGS_NONE, &bytes_sent);
    if(result == CY_RSLT_SUCCESS)
    {
        printf("Acknowledgement sent to TCP server\n");
    }
    
    print_heap_usage("After controlling the LED and ACKing server\n");

    return result;
}

/*******************************************************************************
 * Function Name: tcp_disconnection_handler
 *******************************************************************************
 * Summary:
 *  Callback function to handle TCP client disconnection event.
 *
 * Parameters:
 * cy_socket_t socket_handle: Connection handle for the TCP client socket
 *  void *args : Parameter passed on to the function (unused)
 *
 * Return:
 *  cy_result result: Result of the operation
 *
 *******************************************************************************/
cy_rslt_t tcp_disconnection_handler(cy_socket_t socket_handle, void *arg)
{
    cy_rslt_t result;

    /* Disconnect the TCP client. */
    result = cy_socket_disconnect(socket_handle, 0);
    
    /* Free the resources allocated to the socket. */
    cy_socket_delete(socket_handle);

    printf("Disconnected from the TCP server!\n");

    /* Give the semaphore so as to connect to TCP server.  */
    xSemaphoreGive(connect_to_server);

    return result;
}

/*******************************************************************************
 * Function Name: read_uart_input
 *******************************************************************************
 * Summary:
 *  Function to read user input from UART terminal.
 *
 * Parameters:
 *  uint8_t* input_buffer_ptr: Pointer to input buffer
 *
 * Return:
 *  None
 *
 *******************************************************************************/
void read_uart_input(uint8_t* input_buffer_ptr)
{
    cy_rslt_t result = CY_RSLT_SUCCESS;    
    uint8_t *ptr = input_buffer_ptr; 
    uint32_t numBytes;

    do
    {
        /* Check for data in the UART buffer with zero timeout. */
        numBytes = cyhal_uart_readable(&cy_retarget_io_uart_obj);

        if(numBytes > 0)
        {
            result = cyhal_uart_getc(&cy_retarget_io_uart_obj, ptr,
                            UART_INPUT_TIMEOUT_MS);

            if(result == CY_RSLT_SUCCESS)
            {
                if((*ptr == '\r') || (*ptr == '\n'))
                {
                    printf("\n");
                }
                else
                {
                    /* Echo the received character */
                    cyhal_uart_putc(&cy_retarget_io_uart_obj, *ptr);

                    if (*ptr != '\b')
                    {
                        ptr++;
                    }
                    else if(ptr != input_buffer_ptr)
                    {
                        ptr--;
                    }
                }
            }
        }

        vTaskDelay(RTOS_TICK_TO_WAIT);

    } while((*ptr != '\r') && (*ptr != '\n'));

    /* Terminate string with NULL character. */
    *ptr = '\0';
}

/* [] END OF FILE */
