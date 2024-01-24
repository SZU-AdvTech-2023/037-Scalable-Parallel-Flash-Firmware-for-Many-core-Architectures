#include "xil_types.h"
#include "xil_exception.h"

/* intr.c */
int intr_setup(void);
void intr_enable(void);
void intr_disable(void);
int intr_setup_irq(u16 intr_id, int trigger_type, Xil_InterruptHandler handler,
                   void* cb_data);
int intr_enable_irq(u16 intr_id);
int intr_disable_irq(u16 intr_id);
void intr_disconnect_irq(u16 intr_id);
unsigned long intr_save(void);
void intr_restore(unsigned long flags);

int ipi_setup(void);
void ipi_clear_status(void);
