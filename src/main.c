#include <stdio.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

// --- Definições de Pinos e Constantes ---
#define VRX_PIN 27              // ADC1 (Eixo horizontal do joystick)
#define VRY_PIN 26              // ADC0 (Eixo vertical do joystick)

#define BUTTON_PIN_A 5          // CORRIGIDO: Botão A (SW) - Simula Usuário A
#define BUTTON_PIN_B 6          // CORRIGIDO: Botão B - Simula Usuário B

#define BUZZER_PIN 21           // Saída digital – Buzzer passivo
#define LED_RGB_PIN_G 11        // LED RGB Verde (Sinal de Acesso OK)
#define LED_RGB_PIN_R 13        // LED RGB Vermelho (Sinal de ACESSO NEGADO)

#define NUM_USUARIOS_BARREIRA 2 // Limite da barreira (Botão A + Botão B)
const uint16_t PWM_WRAP_VALUE = 249;


// --- Handles e Recursos Globais ---
QueueHandle_t joystick_button_queue;    
SemaphoreHandle_t usb_mutex;            
SemaphoreHandle_t mutex_contador;       
SemaphoreHandle_t mutex_credencial;     
SemaphoreHandle_t barreira_semaforo;    

// Variáveis de Estado Global (Recursos Críticos)
volatile int contador_chegadas = 0;      
volatile bool credencial_ok = false;     
uint slice_num_buzzer;                  

// --- Estrutura para dados da fila ---
typedef struct {
    uint8_t origem;     
    uint16_t valor_x;   
    uint16_t valor_y;   
} DadoFila_t;

// --- Função Auxiliar para Log Protegido ---
void safe_print(const char *msg) {
    if (xSemaphoreTake(usb_mutex, portMAX_DELAY) == pdTRUE) {
        printf("%s\n", msg);
        xSemaphoreGive(usb_mutex);
    }
}


// ----------------------------------------------------------------------
// --- TAREFA 1: Produtor (Leitura do Joystick - Credencial) ---
// ----------------------------------------------------------------------
void sensor_joystick_task(void *param) {
    adc_init();
    adc_gpio_init(VRX_PIN);
    adc_gpio_init(VRY_PIN);

    DadoFila_t dado_joystick = {.origem = 0};

    while (1) {
        adc_select_input(1);
        dado_joystick.valor_x = adc_read();
        adc_select_input(0);
        dado_joystick.valor_y = adc_read();

        xQueueSend(joystick_button_queue, &dado_joystick, 0);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ----------------------------------------------------------------------
// --- TAREFA 2: Barreira (Usuário A ou B) ---
// ----------------------------------------------------------------------
void button_barrier_task(void *param) {
    uint pin = (uint)param;
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);

    bool last_state = true; 
    const char *user_name = (pin == BUTTON_PIN_A) ? "Usuario A (5)" : "Usuario B (6)";

    while (1) {
        bool current_state = gpio_get(pin);

        // Detecta borda de descida (pressionado)
        if (last_state == true && current_state == false) {
            
            // --- INÍCIO DA SEÇÃO CRÍTICA DA BARREIRA (MUTEX) ---
            if (xSemaphoreTake(mutex_contador, portMAX_DELAY) == pdTRUE) {
                
                if (contador_chegadas < NUM_USUARIOS_BARREIRA) {
                    contador_chegadas++;

                    char msg[64];
                    snprintf(msg, sizeof(msg), "%s chegou na Barreira. Total: %d", user_name, contador_chegadas);
                    safe_print(msg);
                    
                    if (contador_chegadas == NUM_USUARIOS_BARREIRA) {
                        // ÚLTIMO A CHEGAR: Libera a Barreira (Semáforo Binário)
                        xSemaphoreGive(barreira_semaforo); 
                        safe_print(">> BARREIRA LIBERADA PELA ULTIMA PESSOA! <<");
                    }
                }
                xSemaphoreGive(mutex_contador); // FIM DA SEÇÃO CRÍTICA DO MUTEX
            }
            
            vTaskDelay(pdMS_TO_TICKS(200)); // Debounce
        }
        last_state = current_state;

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}


// ----------------------------------------------------------------------
// --- TAREFA 3: Consumidor (Verificação de Credencial) ---
// ----------------------------------------------------------------------
void credential_verifier_task(void *param) {
    DadoFila_t dado_recebido;

    while (1) {
        if (xQueueReceive(joystick_button_queue, &dado_recebido, portMAX_DELAY) == pdTRUE) {
            
            if (dado_recebido.origem == 0) { // Joystick data
                bool senha_valida = false;
                
                // Lógica de Senha de Posição (Centralizado no Joystick: 1500 a 2500)
                if (dado_recebido.valor_x > 1500 && dado_recebido.valor_x < 2500 &&
                    dado_recebido.valor_y > 1500 && dado_recebido.valor_y < 2500) {
                    senha_valida = true;
                }

                // --- ATUALIZAÇÃO DO RECURSO CRÍTICO: credencial_ok ---
                if (xSemaphoreTake(mutex_credencial, 0) == pdTRUE) {
                    credencial_ok = senha_valida;
                    xSemaphoreGive(mutex_credencial);
                }

                // Log de Status
                if (xSemaphoreTake(usb_mutex, 0) == pdTRUE) {
                    if (!senha_valida) {
                        printf("Status: ERRO DE CREDENCIAL. Posicione o joystick no centro.\n");
                    } else {
                        printf("Status: CREDENCIAL OK. Aguardando sincronizacao...\n");
                    }
                    xSemaphoreGive(usb_mutex);
                }
            }
        }
    }
}


// ----------------------------------------------------------------------
// --- TAREFA 4: Atuador Final (Liberação do Acesso - Resposta em Tempo Real) ---
// ----------------------------------------------------------------------
void final_actuator_task(void *param) {
    uint channel = pwm_gpio_to_channel(BUZZER_PIN);

    // Inicialização e Desligamento Inicial dos LEDs
    gpio_init(LED_RGB_PIN_G);
    gpio_set_dir(LED_RGB_PIN_G, GPIO_OUT);
    gpio_put(LED_RGB_PIN_G, 0); 
    gpio_init(LED_RGB_PIN_R); 
    gpio_set_dir(LED_RGB_PIN_R, GPIO_OUT);
    gpio_put(LED_RGB_PIN_R, 0); 
    
    // Configuração do Buzzer
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    slice_num_buzzer = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_wrap(slice_num_buzzer, PWM_WRAP_VALUE);
    pwm_set_clkdiv_int_frac(slice_num_buzzer, 250, 0);
    pwm_set_enabled(slice_num_buzzer, true);
    pwm_set_chan_level(slice_num_buzzer, channel, 0); 

    while (1) {
        // BLOQUEIA AQUI: Espera pelo Semáforo da Barreira
        if (xSemaphoreTake(barreira_semaforo, portMAX_DELAY) == pdTRUE) {
            
            // --- CHECAGEM FINAL (Dentro do Deadline de Resposta) ---
            bool acesso_liberado = false;
            
            if (xSemaphoreTake(mutex_credencial, 0) == pdTRUE) {
                acesso_liberado = credencial_ok; 
                xSemaphoreGive(mutex_credencial);
            }
            
            // --- ATUAÇÃO FINAL ---
            if (acesso_liberado) {
                // SUCESSO: LED Verde e Buzzer
                safe_print(">> ACESSO CONCEDIDO! Resposta em Tempo Real: Buzzer e LED OK. <<");
                
                gpio_put(LED_RGB_PIN_G, 1); 
                pwm_set_chan_level(slice_num_buzzer, channel, PWM_WRAP_VALUE / 2);
                
                vTaskDelay(pdMS_TO_TICKS(1000)); 
                
                gpio_put(LED_RGB_PIN_G, 0);
                pwm_set_chan_level(slice_num_buzzer, channel, 0);

            } else {
                // NEGADO: LED Vermelho
                safe_print(">> ACESSO NEGADO: Credencial Incorreta. <<");

                gpio_put(LED_RGB_PIN_R, 1); 
                
                vTaskDelay(pdMS_TO_TICKS(1000));
                
                gpio_put(LED_RGB_PIN_R, 0); 
            }
            
            // --- RESETA O ESTADO PARA O PRÓXIMO CICLO ---
            if (xSemaphoreTake(mutex_contador, 0) == pdTRUE) {
                contador_chegadas = 0;
                xSemaphoreGive(mutex_contador);
            }
        }
    }
}


// ----------------------------------------------------------------------
// --- Função principal (main) ---
// ----------------------------------------------------------------------
int main() {
    stdio_init_all();
    safe_print("Sistema de Check-in Bi-Metrico (RTOS) Inicializado.");

    // --- Criação dos Recursos ---
    joystick_button_queue = xQueueCreate(10, sizeof(DadoFila_t));   
    usb_mutex = xSemaphoreCreateMutex();                             
    mutex_contador = xSemaphoreCreateMutex();                        
    mutex_credencial = xSemaphoreCreateMutex();                      
    barreira_semaforo = xSemaphoreCreateBinary();                    

    if (joystick_button_queue == NULL || usb_mutex == NULL || barreira_semaforo == NULL) {
        safe_print("ERRO FATAL: Falha na criacao de recursos FreeRTOS!");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // --- Criação das Tarefas (Threads) ---
    // Prioridade: 4 (mais alta) -> 1 (mais baixa)
    
    // TAREFA 4: Atuador Final (Resposta em Tempo Real - Mais Crítica)
    xTaskCreate(final_actuator_task, "Final_Actuator_RT", configMINIMAL_STACK_SIZE * 2, NULL, 4, NULL); 
    
    // TAREFA 3: Consumidor (Verificação de Credencial - Lógica)
    xTaskCreate(credential_verifier_task, "Cred_Verify_Cons", configMINIMAL_STACK_SIZE * 2, NULL, 3, NULL); 
    
    // TAREFA 2: Barreira Usuário A (Botão 5)
    xTaskCreate(button_barrier_task, "Barreira_A", configMINIMAL_STACK_SIZE, (void*)BUTTON_PIN_A, 2, NULL); 
    
    // TAREFA 2: Barreira Usuário B (Botão 6)
    xTaskCreate(button_barrier_task, "Barreira_B", configMINIMAL_STACK_SIZE, (void*)BUTTON_PIN_B, 2, NULL); 

    // TAREFA 1: Produtor (Joystick - Entrada de Credencial)
    xTaskCreate(sensor_joystick_task, "Joystick_Prod", configMINIMAL_STACK_SIZE, NULL, 1, NULL); 

    // Inicia o escalonador do FreeRTOS
    vTaskStartScheduler();

    while (true) {} 

    return 0;
}