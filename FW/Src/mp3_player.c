/*****************************************************************************
  * @file    mp3_player.c
  * @author  Arash Golgol
  * @brief   MP3 player main tasks and functions
*****************************************************************************/
/* FreeRTOS headers */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "cmsis_os.h"

/* HAL headers */
#include "main.h"
#include "spi.h"

/* General headers */
#include <stdint.h>
#include "mp3_player.h"
#include "GUI.h"
#include "vs10xx.h"

/* Global variable */
FATFS 										fs;  				/* FS object for SD card logical drive */
SemaphoreHandle_t					dreq_sem;		/* vs10xx dreq IRQ */
traceString 							sd_chn, vs_chn;				

/**
 * \brief initilize mp3-player and creat related tasks  	
 */
void vsmp3_init(void *vparameters)
{
	BaseType_t									xstatus;
	struct controller_qlist*		qlist;			/* queue list for all tasks */
	
	qlist = 
	(struct controller_qlist* )pvPortMalloc(sizeof(struct controller_qlist));
	configASSERT(qlist);
	
	/* create task command queue */
	qlist->blink  = xQueueCreate(1, sizeof(struct mp3p_cmd));
	qlist->vs10xx = xQueueCreate(2, sizeof(struct mp3p_cmd));
	qlist->sdcard = xQueueCreate(2, sizeof(struct mp3p_cmd));
	
	
	/* create blink tasks */
	xstatus = 	xTaskCreate(vtask_blink, 
													"blink", 
													TASK_BLINK_STACK_SIZE, 
													qlist, 			/* command queue to blink task */
													TASK_BLINK_PRIORITY, 
													NULL);
	configASSERT(xstatus == pdPASS);
	
	/* create controller task */
	xstatus = 	xTaskCreate(vtask_controller,
													"controller",
													TASK_CONTROLLER_STACK_SIZE,
													qlist,		/* list of command queues to each tasks */				
													TASK_CONTROLLER_PRIORITY, 
													NULL);
	configASSERT(xstatus == pdPASS);
	
	/* create vs10xx task */
	xstatus = 	xTaskCreate(vtask_vs10xx,
													"vs10xx",
													TASK_VS10XX_STACK_SIZE,
													qlist,
													TASK_VS10XX_PRIORITY,
													NULL);
	configASSERT(xstatus == pdPASS);
	
	/* create sdcard streaming buffer task */
	xstatus = 	xTaskCreate(vtask_sdcard,
													"sdcard",
													TASK_SDCARD_STACK_SIZE,
													qlist,
													TASK_SDCARD_PRIORITY,
													NULL);
	configASSERT(xstatus == pdPASS);
	
	/* debug */
	debug_print("task creation done\r\n");
	
	/* create binary semaphore for VS10xx DREQ interrup */
	dreq_sem = xSemaphoreCreateBinary();
	configASSERT(dreq_sem);
	
	/* mount SD card */
	if (f_mount(&fs,"", 0	) != FR_OK) {
		debug_print("f_mount failed\r\n");
	}
	
	/* init vs1063 */
	vs_setup(&hspi1);
	
	sd_chn = xTraceRegisterString("sdcard");
	vs_chn = xTraceRegisterString("vs10xx");
	
	/* Start the scheduler. */
	vTaskStartScheduler();

	/* Will only get here if there was not enough heap space to create the
	   idle task. */
}

/**
 * \brief main system controller   	
 */
void vtask_controller(void* vparameters)
{
	struct controller_qlist* 	pqlist 	= vparameters;	/* command queue */
	struct mp3p_cmd						qcmd;
	struct stream_buff*				sbuff;
	uint8_t*									buff;
	FRESULT										result;
	
	/* initlizie file and stream buffer */
	sbuff = (struct stream_buff *)pvPortMalloc(sizeof(struct stream_buff));
	configASSERT(sbuff);
	sbuff->qwrite = pqlist->sdcard;
	result = f_open(&sbuff->file, "Poker.mp3", FA_READ);
	configASSERT(result == FR_OK);
	
	/* initlizie lwrb */
	buff  = (uint8_t *)pvPortMalloc(STREAM_BUFF_SIZE);
	configASSERT(buff);
	lwrb_init(&sbuff->lwrb, buff, STREAM_BUFF_SIZE);
	lwrb_set_evt_fn(&sbuff->lwrb, sd_buff_evt_fn);
	
	debug_print("free heap: %zu\r\n",xPortGetFreeHeapSize());
	
	/* create SD card data stream buffer */
	qcmd.cmd = CMD_SDCARD_START_READ;
	qcmd.arg = (uintptr_t) sbuff;
	xQueueSend(pqlist->sdcard, &qcmd, 0);
	
	/* start playing mp3 file */
	qcmd.cmd = CMD_VS10XX_PLAY;
	qcmd.arg = (uintptr_t) sbuff;
	xQueueSend(pqlist->vs10xx, &qcmd, 0);
	
	/* start playing mp3 file */
	qcmd.cmd = CMD_LED_BLINK_SET;
	qcmd.arg = 1000;
	xQueueSend(pqlist->blink, &qcmd, 0);
	
	/* main loop */
	for(;;) {
		vTaskDelay(pdMS_TO_TICKS(10000));
	} /* for(;;) */
}


/**
 * \brief blink led 	
 */
void vtask_blink(void* vparameters)
{
	struct controller_qlist* 	pqlist 	= vparameters;	/* command queue */
	struct mp3p_cmd						blink_cmd;
	BaseType_t 								xstatus;
	BaseType_t 								delay_ms = 500;    /*if no command is send ever */
	
	/* main loop */
	for(;;) {
		/* don't wait for command */
		xstatus = xQueueReceive(pqlist->blink, &blink_cmd, 0);	
		/* new command is available */
		if (xstatus == pdPASS) {		
			debug_print("cmd: %02x, arg: %02x\r\n", blink_cmd.cmd, blink_cmd.arg);
			if (blink_cmd.cmd == CMD_LED_BLINK_SET && blink_cmd.arg != 0) {
				delay_ms = blink_cmd.arg;
			}
		} /* if (xstatus == pdPASS) */
		
		vTaskDelay(pdMS_TO_TICKS(delay_ms));
		HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
	} /* for(;;) */
}

/**
 * \brief VS1063A controller	
 */
void vtask_vs10xx(void* vparameters)
{
	struct controller_qlist* 	pqlist 	= vparameters;	/* command queue */
	struct mp3p_cmd 					vs10xx_cmd;
	lwrb_t*										lwrb;
	struct stream_buff*				sbuff;
	BaseType_t 								xstatus;
	static BaseType_t					vs_status;
	void*											buff;
	
	/* main loop */
	for(;;) {
		/* don't wait for command */
		xstatus = xQueueReceive(pqlist->vs10xx, &vs10xx_cmd, 0);
		if (xstatus == pdPASS) {
		//debug_print("cmd: %02x, arg: %p\r\n", vs10xx_cmd.cmd 
		//																		,	(void *) vs10xx_cmd.arg);
			sbuff = (struct stream_buff *)vs10xx_cmd.arg;
			switch (vs10xx_cmd.cmd) {
				case CMD_VS10XX_PLAY:
					lwrb = &sbuff->lwrb;
					vs_status = 0x01;
				break;
				
				case CMD_VS10XX_STOP:
					vs_status = 0x00;
					break;
				
				default:
					break; 
			} /* switch (vs10xx_cmd.cmd) */
		} /* if (xstatus == pdPASS) */
		
		/* playing */
		if (vs_status == 0x01) {
			/* on DREQ rising edge, it can accept at last 32 byte of data */
			if(HAL_GPIO_ReadPin(VS_DREQ_PORT, VS_DREQ) == GPIO_PIN_SET) {
				buff = lwrb_get_linear_block_read_address(lwrb);
				HAL_SPI_Transmit(&hspi1, (uint8_t* )buff, 32 , 0xFFFF);
				lwrb_skip(lwrb, 32);
				//vTracePrintF(vs_chn,"lwrb-full: %d", lwrb_get_linear_block_read_length(lwrb));
				//vTracePrintF(vs_chn,"lwrb->r: %d, lwrb->w: %d", lwrb->r, lwrb->w);			
			} else {
				/* wait for DREQ */
				xSemaphoreTake(dreq_sem, portMAX_DELAY);
			} /* if(dreq == 1) */
		} /* if (vs_status == 0x01) */			
	} /* for(;;) */
}

/**
 * \breif SD-card data streamer
 */
void vtask_sdcard(void* vparameters)
{	
	struct controller_qlist* 	pqlist 	= vparameters;	/* command queue */
	struct mp3p_cmd 					sd_cmd;
	struct stream_buff*				sbuff;
	FIL*											file;
	lwrb_t*										lwrb;
	FRESULT										result;
	uint32_t 									read_len;
	void*											buff;
	size_t 										len;		
	
	/* main loop */
	for(;;) {
		/* wait for command queue */
		xQueueReceive(pqlist->sdcard, &sd_cmd, portMAX_DELAY);
		//debug_print("cmd: %02x, arg: %p\r\n", sd_cmd.cmd, (void *) sd_cmd.arg);
		
		sbuff = (struct stream_buff *)sd_cmd.arg;
		switch(sd_cmd.cmd) {
			case CMD_SDCARD_START_READ:		/* file and lwrb are valid */
				file = &sbuff->file;
				lwrb = &sbuff->lwrb;
			break;
					
			case CMD_SDCARD_CONT_READ:		/* only lwrb is valid */
				/* if end of file is reached we can't read more data */
				if (f_eof(file)) {		
					continue;
				} else {
					lwrb = &sbuff->lwrb;
					break;
				}
								
			case CMD_SDCARD_STOP_READ:		/* file and lwrb are valid */
			/* end of streaming */
				f_close(&sbuff->file);
				lwrb_reset(&sbuff->lwrb);
			continue;
		} /* switch(sd_cmd.cmd) */
		
		/* writing data to stream buffer (keep it full) */
		len = lwrb_get_linear_block_write_length(lwrb);
		buff = lwrb_get_linear_block_write_address(lwrb);
		result = f_read(file, buff, len, &read_len);
		if (result == FR_OK) {
			lwrb_advance(lwrb, read_len);
			//debug_print("write %d bytes @ %p\r\n", read_len, (void *)buff);
			//vTracePrintF(sd_chn, "len: %d, read_len: %d",len, read_len);
			//vTracePrintF(sd_chn, "lwrb-full: %d", lwrb_get_full(lwrb));
			//vTracePrintF(sd_chn, "lwrb->r: %d, lwrb->w: %d", lwrb->r, lwrb->w);
		} 
	} /* for(;;) */
}

/**
 * \brief           Buffer event function
 */
static void sd_buff_evt_fn(lwrb_t* buff, lwrb_evt_type_t type, size_t len) {	
	struct stream_buff*		sbuff;
	struct mp3p_cmd 			sd_cmd;
	
	sbuff = container_of(buff, struct stream_buff, lwrb);
	
	switch (type) {
		case LWRB_EVT_RESET:
			break;
		
		case LWRB_EVT_READ:
			vTracePrintF(sd_chn, "[EVT] Buffer read event: %d byte(s)!\r\n", (int)len);
			if (lwrb_get_linear_block_read_length(buff) < STREAM_BUFF_HALF_SIZE ) {
				/* we need more data in stream buffer */
				sd_cmd.cmd = CMD_SDCARD_CONT_READ;
				sd_cmd.arg = (uintptr_t) sbuff;
				xQueueSend(sbuff->qwrite, &sd_cmd, 0);
			}	 /* if (lwrb_get_full(lwrb) >= STREAM_BUFF_HALF_SIZE ) */
		break;
    
		case LWRB_EVT_WRITE:
			vTracePrintF(sd_chn, "[EVT] Buffer write event: %d byte(s)!\r\n", (int)len);
		break;
    
		default: break;
	}
}
/* end of file */
