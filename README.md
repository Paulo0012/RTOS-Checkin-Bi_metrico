# RTOS-Senha-Sequencial-Temporal

## 🔒 Sistema de Controle de Acesso Bi-Métrico com Deadline Temporal

Este projeto demonstra a aplicação rigorosa de conceitos de **Sistemas Operacionais de Tempo Real (RTOS)** e **programação multithread** na plataforma **Raspberry Pi Pico W (BitDogLab)**.

O sistema simula um controle de acesso que exige o cumprimento de dois fatores (bi-métrico) para a liberação:

1.  **Credencial Lógica (Joystick):** A posição do Joystick deve estar no centro.
2.  **Sequência Temporal (Botões A → B):** O Botão B deve ser pressionado em no máximo **2 segundos** após o Botão A ser pressionado.

---

### ⚙️ Arquitetura e Mecanismos de Concorrência

O projeto é estruturado em tarefas concorrentes (Threads) que utilizam os seguintes primitivos de sincronização:

| Mecanismo | Uso no Projeto | Finalidade |
| :--- | :--- | :--- |
| **Mutex (`mutex_time_state`)** | Proteção da variável de estado (`tick_start_time` e `credencial_ok`). | Garante a **Exclusão Mútua** na marcação do tempo de início da sequência, eliminando *race conditions*. |
| **Semáforo** (`final_trigger_sem`) | Sinalização entre a Tarefa B (Fiscalização) e a Tarefa 4 (Atuador). | Desbloqueia o Atuador de Alta Prioridade para iniciar a **Resposta em Tempo Real**. |
| **Medição de Tick** (`xTaskGetTickCount`) | Fiscalização do **Deadline de 2 segundos**. | Fornece a base temporal precisa para calcular se o tempo decorrido entre os cliques A e B está dentro do limite. |
| **Fila** (`temporal_result_queue`) | Comunicação entre Tarefas. | Transporta o resultado da checagem temporal (`pdTRUE`/`pdFALSE`) para a Tarefa Atuadora (Prioridade 4). |

---

### 🧪 Instruções de Teste Rápido

O sistema só concederá **ACESSO CONCEDIDO** (LED Verde + Buzzer) se **TODAS** as três condições a seguir forem verdadeiras no momento do clique do Botão B:

1.  **Credencial OK:** Joystick posicionado e mantido no **Centro** (Valores entre 1500 e 2500).
2.  **Ordem OK:** Botão A deve ser pressionado primeiro.
3.  **Deadline OK:** Botão B deve ser pressionado em **menos de 2 segundos** após o clique do Botão A.

| Condição | Resultado Esperado |
| :--- | :--- |
| **SUCESSO TOTAL** | LED Verde 🟢 e Buzzer 🔊. |
| **Falha de Deadline (Lenta)** | LED Vermelho 🔴. Log: `DEADLINE (A->B em 2s) EXPIRADO.` |
| **Falha de Credencial** | LED Vermelho 🔴. Log: `Credencial (Joystick) Incorreta.` |