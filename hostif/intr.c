#include "xparameters.h"
#include "xil_exception.h"
#include "xdebug.h"
#include "xil_io.h"
#include "xscugic.h"
#include "xipipsu.h"

#include "intr.h"

#define INTC_DEVICE_ID XPAR_SCUGIC_SINGLE_DEVICE_ID
#define INTC           XScuGic
#define INTC_HANDLER   XScuGic_InterruptHandler

static INTC Intc; /* Instance of the Interrupt Controller */

int intr_setup(void)
{
    int Status;
    XScuGic_Config* IntcConfig;
    INTC* IntcInstancePtr = &Intc;

    IntcConfig = XScuGic_LookupConfig(INTC_DEVICE_ID);
    if (NULL == IntcConfig) {
        return XST_FAILURE;
    }

    Status = XScuGic_CfgInitialize(IntcInstancePtr, IntcConfig,
                                   IntcConfig->CpuBaseAddress);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    /* Enable interrupts from the hardware */
    Xil_ExceptionInit();
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
                                 (Xil_ExceptionHandler)INTC_HANDLER,
                                 (void*)IntcInstancePtr);

    return XST_SUCCESS;
}

int intr_setup_irq(u16 intr_id, int trigger_type, Xil_InterruptHandler handler,
                   void* cb_data)
{
    INTC* IntcInstancePtr = &Intc;

    XScuGic_SetPriorityTriggerType(IntcInstancePtr, intr_id, 0xA0,
                                   trigger_type);
    return XScuGic_Connect(IntcInstancePtr, intr_id, handler, cb_data);
}

void intr_enable(void) { Xil_ExceptionEnable(); }

void intr_disable(void) { Xil_ExceptionDisable(); }

int intr_enable_irq(u16 intr_id)
{
    XScuGic_Enable(&Intc, intr_id);
    return XST_SUCCESS;
}

int intr_disable_irq(u16 intr_id)
{
    XScuGic_Disable(&Intc, intr_id);
    return XST_SUCCESS;
}

void intr_disconnect_irq(u16 intr_id) { XScuGic_Disconnect(&Intc, intr_id); }

unsigned long intr_save(void) { return mfcpsr(); }

void intr_restore(unsigned long flags)
{
    if (flags & XIL_EXCEPTION_IRQ)
        intr_disable();
    else
        intr_enable();
}

static XIpiPsu ipi_psu_inst;

static void ipi_handler(void* callback)
{
    XIpiPsu* inst_ptr = (XIpiPsu*)callback;
    u32 src_mask;

    src_mask = XIpiPsu_GetInterruptStatus(inst_ptr);
    XIpiPsu_ClearInterruptStatus(inst_ptr, src_mask);
}

int ipi_setup(void)
{
    int status;
    XIpiPsu_Config* config;

    config = XIpiPsu_LookupConfig(XPAR_XIPIPSU_0_DEVICE_ID);
    if (config == NULL) {
        xil_printf("IPI PSU not found\r\n");
        return XST_FAILURE;
    }

    status = XIpiPsu_CfgInitialize(&ipi_psu_inst, config, config->BaseAddress);
    if (status != XST_SUCCESS) {
        xil_printf("IPI config failed\r\n");
        return XST_FAILURE;
    }

    status = intr_setup_irq(XPAR_XIPIPSU_0_INT_ID, 0x1,
                            (Xil_InterruptHandler)ipi_handler, &ipi_psu_inst);
    if (status != XST_SUCCESS) {
        xil_printf("Failed to set up IPI IRQ\r\n");
        return XST_FAILURE;
    }

    intr_enable_irq(XPAR_XIPIPSU_0_INT_ID);

    /* Enable IPI from RPUs. */
    XIpiPsu_InterruptEnable(&ipi_psu_inst, XIPIPSU_ALL_MASK);
    XIpiPsu_ClearInterruptStatus(&ipi_psu_inst, XIPIPSU_ALL_MASK);

    return XST_SUCCESS;
}

void ipi_clear_status(void)
{
    XIpiPsu_ClearInterruptStatus(&ipi_psu_inst, XIPIPSU_ALL_MASK);
}
