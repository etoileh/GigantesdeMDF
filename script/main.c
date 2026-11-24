/**
 * @file main.c
 * @brief Gigante de MDF - firmware do robo (ATmega328P, Arduino Nano, 16 MHz).
 *
 * Tudo e feito direto nos registradores, sem funcoes do Arduino. Em C puro, o
 * programa comeca em main(), que chama setup() uma vez e loop() sem parar.
 *
 * Perifericos usados:
 *  - Timer1: base de tempo de 1 ms e controle do laser (1 s ligado, 1 s desligado).
 *  - Timer2: PWM dos dois motores (OC2B = motor esquerdo, OC2A = motor direito).
 *  - ADC canal 5: leitura do LDR (receptor do laser).
 *  - USART0: mensagens de depuracao a 115200 baud (somente transmissao).
 *
 * Funcionamento:
 *  - Os quatro botoes movem o robo enquanto exatamente um deles estiver pressionado.
 *  - Quando o LDR fica acima do limiar por LDR_CONFIRMACOES leituras seguidas, o robo
 *    perde uma vida, gira por TEMPO_GIRO_MS, para e fica TEMPO_PROTECAO_MS sem reagir
 *    (motores e leitura do LDR desativados).
 *  - Sem vidas, o robo fica parado ate o botao de reset ser apertado.
 *
 * Mapeamento de pinos conforme o esquematico v2.0.
 */
#ifndef F_CPU
/** Clock do microcontrolador, em Hz. */
#define F_CPU 16000000UL
#endif

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/atomic.h>
#include <stdint.h>

// =====================================
// PINOS
// =====================================

// Motor esquerdo
/** @name Motor esquerdo - L293D, PORTD
 *  @{ */
/** D3 - EN1,2 do L293D, PWM (OC2B). */
#define PWM_ESQ PD3       // D3
/** D2 - entrada 1A do L293D. */
#define ESQ_1 PD2         // D2
/** D4 - entrada 2A do L293D. */
#define ESQ_2 PD4         // D4
/** @} */

// Motor direito
/** @name Motor direito - L293D, PORTB
 *  @{ */
/** D11 - EN3,4 do L293D, PWM (OC2A). */
#define PWM_DIR PB3       // D11
/** D10 - entrada 3A do L293D. */
#define DIR_1 PB2         // D10
/** D8 - entrada 4A do L293D. */
#define DIR_2 PB0         // D8
/** @} */

// Botoes
/** @name Botoes (pull-up interno, pressionado = nivel baixo)
 *  @{ */
/** A2 (PORTC) - botao de frente. */
#define BTN_FRENTE PC2    // A2
/** A1 (PORTC) - botao de re. */
#define BTN_TRAS PC1      // A1
/** A4 (PORTC) - botao de giro para a esquerda. */
#define BTN_ESQUERDA PC4  // A4
/** A3 (PORTC) - botao de giro para a direita. */
#define BTN_DIREITA PC3   // A3
/** D12 (PORTB) - botao de reset das vidas. */
#define BTN_RESET PB4     // D12
/** @} */

// Sensor e laser
/** @name Sensor e laser
 *  @{ */
/** A5 - canal do ADC ligado ao LDR (entrada analogica, sem pull-up). */
#define LDR_CANAL 5       // A5
/** D9 (PORTB) - saida do laser. */
#define LASER PB1         // D9
/** @} */

// LEDs de vida
/** @name LEDs de vida (PORTD)
 *  @{ */
/** D5 - acende com 1 vida ou mais. */
#define LED_V1 PD5        // D5
/** D6 - acende com 2 vidas ou mais. */
#define LED_V2 PD6        // D6
/** D7 - acende com 3 vidas. */
#define LED_V3 PD7        // D7
/** @} */

// =====================================
// CONFIGURACOES
// =====================================

/** Duty do PWM dos motores, de 0 a 255. */
#define VELOCIDADE 200

// AJUSTAR APOS TESTE FISICO
/** Tempo do giro automatico apos um acerto, em ms (ajustar apos teste fisico). */
#define TEMPO_GIRO_MS 500UL

// Protecao total desde o dano
/** Tempo, contado a partir do acerto, com motores e leitura do LDR desativados, em ms. */
#define TEMPO_PROTECAO_MS 5000UL

// Laser: 1 segundo ligado / 1 desligado
/** Tempo ligado e tempo desligado do laser, em ms. */
#define LASER_PERIODO_MS 1000U

// Valores medidos na PCB
/** Leitura do ADC (0..1023) a partir da qual ha laser (valor medido na PCB). */
#define LDR_LIMIAR_DISPARO 800U

// 6 amostras separadas por 5 ms
/** Leituras seguidas acima do limiar necessarias para confirmar o acerto. */
#define LDR_CONFIRMACOES 6U
/** Intervalo entre duas leituras do LDR, em ms. */
#define LDR_INTERVALO_MS 5UL

/** Tempo que a leitura de um botao de movimento precisa ficar estavel, em ms. */
#define DEBOUNCE_MS 20UL
/** Tempo que a leitura do botao de reset precisa ficar estavel, em ms. */
#define DEBOUNCE_RESET_MS 40UL

/** Intervalo entre as mensagens de depuracao do LDR, em ms. */
#define SERIAL_INTERVALO_MS 100UL

// =====================================
// ESTADOS
// =====================================

/** Estados da maquina de estados do jogo. */
enum Estado {
    /** Controle manual e leitura do LDR ligados. */
    ATIVO,
    /** Giro automatico e pausa de protecao apos um acerto. */
    ATINGIDO,
    /** Sem vidas: aguarda o botao de reset. */
    ELIMINADO
};

/** Estado atual da maquina de estados. */
enum Estado estado = ATIVO;

/** Vidas restantes. */
uint8_t vidas = 3;
/** Instante do ultimo acerto, em ms. */
uint32_t inicioDano = 0;

// =====================================
// TIMER1 - 1 MS + LASER
// =====================================

/** Milissegundos desde o boot. Alterada apenas na interrupcao do Timer1. */
volatile uint32_t tempoMs = 0;
/** Milissegundos desde a ultima inversao do laser. */
volatile uint16_t contadorLaser = 0;
/** Fase do laser: 1 = ligado, 0 = desligado. */
volatile uint8_t laserLigado = 1;

/**
 * @brief Interrupcao do Timer1 (a cada 1 ms).
 *
 * Incrementa o contador de milissegundos e inverte o laser a cada LASER_PERIODO_MS.
 */
ISR(TIMER1_COMPA_vect) {
    tempoMs++;

    contadorLaser++;

    if (contadorLaser >= LASER_PERIODO_MS) {
        contadorLaser = 0;
        laserLigado = !laserLigado;

        if (laserLigado) {
            PORTB |= (1 << LASER);
        } else {
            PORTB &= ~(1 << LASER);
        }
    }
}

/**
 * @brief Le o contador de milissegundos de forma atomica.
 *
 * As interrupcoes ficam desligadas durante a copia, para nao ler o valor de 32 bits
 * pela metade enquanto a interrupcao o altera.
 *
 * @return Milissegundos desde o boot.
 */
uint32_t agoraMs(void) {
    uint32_t valor;

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        valor = tempoMs;
    }

    return valor;
}

/**
 * @brief Configura o Timer1 em CTC para gerar uma interrupcao a cada 1 ms.
 *
 * Com prescaler 64 e OCR1A = 249: 16 MHz / 64 / 250 = 1000 Hz.
 */
void configurarTimer1(void) {
    TCCR1A = 0;
    TCCR1B = 0;
    TCNT1 = 0;

    // 16 MHz / 64 / (249 + 1) = 1000 Hz
    OCR1A = 249;

    TCCR1B = (1 << WGM12) |
             (1 << CS11) |
             (1 << CS10);

    TIMSK1 = (1 << OCIE1A);
}

// =====================================
// SERIAL - 115200
// =====================================

/**
 * @brief Configura a USART0 a 115200 baud, 8N1, somente transmissao.
 *
 * Com U2X0 e UBRR0 = 16 o baud real e de cerca de 117647 (erro de ~2%).
 */
void uartInit(void) {
    UCSR0A = (1 << U2X0);
    UBRR0H = 0;
    UBRR0L = 16;

    UCSR0B = (1 << TXEN0);

    UCSR0C = (1 << UCSZ01) |
             (1 << UCSZ00);
}

/**
 * @brief Envia um caractere pela serial.
 *
 * Espera o registrador de dados ficar livre (UDRE0) e grava o caractere em UDR0.
 *
 * @param c Caractere a enviar.
 */
void uartChar(char c) {
    while (!(UCSR0A & (1 << UDRE0))) {
    }

    UDR0 = c;
}

/**
 * @brief Envia uma string terminada em zero pela serial.
 *
 * @param s Texto a enviar.
 */
void uartTexto(const char *s) {
    while (*s) {
        uartChar(*s++);
    }
}

/**
 * @brief Envia um numero sem sinal em decimal pela serial.
 *
 * @param n Numero de 0 a 65535.
 */
void uartNumero(uint16_t n) {
    char buffer[6];
    uint8_t i = 0;

    if (n == 0) {
        uartChar('0');
        return;
    }

    while (n > 0) {
        buffer[i++] = '0' + (n % 10);
        n /= 10;
    }

    while (i > 0) {
        uartChar(buffer[--i]);
    }
}

/**
 * @brief Envia um texto seguido de quebra de linha (retorno de carro e nova linha).
 *
 * @param s Texto a enviar.
 */
void uartLinha(const char *s) {
    uartTexto(s);
    uartTexto("\r\n");
}

// =====================================
// TIMER2 - FAST PWM
// =====================================

/**
 * @brief Configura o Timer2 em Fast PWM de 8 bits para os dois motores.
 *
 * OC2A (D11) e OC2B (D3) nao invertidos, prescaler 64 (cerca de 976 Hz). O duty
 * comeca em 0 (motores parados).
 */
void configurarPWM(void) {
    TCCR2A = (1 << COM2A1) |
             (1 << COM2B1) |
             (1 << WGM21) |
             (1 << WGM20);

    TCCR2B = (1 << CS22);

    OCR2A = 0;
    OCR2B = 0;
}

// =====================================
// MOTORES
// =====================================

/** Ultimo valor aplicado ao motor esquerdo (999 = nenhum ainda). */
int16_t ultimoEsq = 999;
/** Ultimo valor aplicado ao motor direito (999 = nenhum ainda). */
int16_t ultimoDir = 999;

/**
 * @brief Comanda o motor esquerdo (PORTD e OC2B).
 *
 * Limita v a +-255 e ignora valores repetidos. Zera o PWM antes de trocar o sentido.
 *
 * @param v Velocidade com sinal: positiva = frente, negativa = tras, 0 = parado.
 */
void motorEsquerdo(int16_t v) {
    if (v > 255) v = 255;
    if (v < -255) v = -255;

    if (v == ultimoEsq) return;
    ultimoEsq = v;

    OCR2B = 0;

    if (v > 0) {
        PORTD &= ~(1 << ESQ_2);
        PORTD |= (1 << ESQ_1);
        OCR2B = v;

    } else if (v < 0) {
        PORTD &= ~(1 << ESQ_1);
        PORTD |= (1 << ESQ_2);
        OCR2B = -v;

    } else {
        PORTD &= ~((1 << ESQ_1) |
                   (1 << ESQ_2));
    }
}

/**
 * @brief Comanda o motor direito (PORTB e OC2A).
 *
 * Limita v a +-255 e ignora valores repetidos. Zera o PWM antes de trocar o sentido.
 *
 * @param v Velocidade com sinal: positiva = frente, negativa = tras, 0 = parado.
 */
void motorDireito(int16_t v) {
    if (v > 255) v = 255;
    if (v < -255) v = -255;

    if (v == ultimoDir) return;
    ultimoDir = v;

    OCR2A = 0;

    if (v > 0) {
        PORTB &= ~(1 << DIR_2);
        PORTB |= (1 << DIR_1);
        OCR2A = v;

    } else if (v < 0) {
        PORTB &= ~(1 << DIR_1);
        PORTB |= (1 << DIR_2);
        OCR2A = -v;

    } else {
        PORTB &= ~((1 << DIR_1) |
                   (1 << DIR_2));
    }
}

/**
 * @brief Define a velocidade e o sentido dos dois motores.
 *
 * @param esq Motor esquerdo, de -255 a 255.
 * @param dir Motor direito, de -255 a 255.
 */
void motores(int16_t esq, int16_t dir) {
    motorEsquerdo(esq);
    motorDireito(dir);
}

/**
 * @brief Para os dois motores (entradas do L293D em nivel baixo e PWM em zero).
 */
void pararMotores(void) {
    motores(0, 0);
}

// =====================================
// BOTOES - PULL-UP E DEBOUNCE
// =====================================

// 0 = frente
// 1 = tras
// 2 = esquerda
// 3 = direita
// 4 = reset

/** Mascara (bit no PINC ou no PINB) de cada botao, na ordem dos indices abaixo. */
const uint8_t mascaras[5] = {
    (1 << BTN_FRENTE),
    (1 << BTN_TRAS),
    (1 << BTN_ESQUERDA),
    (1 << BTN_DIREITA),
    (1 << BTN_RESET)
};

/** Ultima leitura bruta de cada botao (1 = solto, 0 = apertado). */
uint8_t brutoAnterior[5];
/** Estado confirmado de cada botao apos o debounce (1 = solto, 0 = apertado). */
uint8_t estavel[5];
/** Instante em que a leitura bruta de cada botao mudou pela ultima vez, em ms. */
uint32_t desdeMudanca[5];

/**
 * @brief Configura os cinco botoes como entrada com pull-up interno.
 *
 * Tambem inicializa os vetores com a leitura atual, para comecar com tudo solto.
 */
void configurarBotoes(void) {
    uint8_t mask =
        (1 << BTN_FRENTE) |
        (1 << BTN_TRAS) |
        (1 << BTN_ESQUERDA) |
        (1 << BTN_DIREITA);

    DDRC &= ~mask;
    PORTC |= mask;

    DDRB &= ~(1 << BTN_RESET);
    PORTB |= (1 << BTN_RESET);

    uint8_t leituraC = PINC;
    uint8_t leituraB = PINB;

    for (uint8_t i = 0; i < 5; i++) {
        uint8_t valor;

        if (i == 4) {
            valor = (leituraB & mascaras[i]) ? 1 : 0;
        } else {
            valor = (leituraC & mascaras[i]) ? 1 : 0;
        }

        brutoAnterior[i] = valor;
        estavel[i] = valor;
        desdeMudanca[i] = 0;
    }
}

/**
 * @brief Faz o debounce de um botao.
 *
 * A leitura so vira o estado estavel depois de ficar igual por DEBOUNCE_MS
 * (DEBOUNCE_RESET_MS no reset).
 *
 * @param indice Botao: 0 = frente, 1 = tras, 2 = esquerda, 3 = direita, 4 = reset.
 * @param leitura Leitura bruta do pino (1 = solto, 0 = apertado).
 * @param agora Tempo atual, em ms.
 * @return 1 se o estado estavel do botao acabou de mudar, 0 caso contrario.
 */
uint8_t atualizarBotao(
    uint8_t indice,
    uint8_t leitura,
    uint32_t agora
) {
    uint16_t debounce =
        (indice == 4) ?
        DEBOUNCE_RESET_MS :
        DEBOUNCE_MS;

    if (leitura != brutoAnterior[indice]) {
        brutoAnterior[indice] = leitura;
        desdeMudanca[indice] = agora;
    }

    if (leitura != estavel[indice] &&
        (uint32_t)(agora - desdeMudanca[indice]) >= debounce) {

        estavel[indice] = leitura;
        return 1;
    }

    return 0;
}

/**
 * @brief Le os botoes e atualiza o estado estavel de cada um.
 *
 * @param agora Tempo atual, em ms.
 * @return 1 se o estado estavel do botao de reset acabou de mudar, 0 caso contrario.
 */
uint8_t atualizarBotoes(uint32_t agora) {
    uint8_t leituraC = PINC;
    uint8_t leituraB = PINB;

    for (uint8_t i = 0; i < 4; i++) {
        uint8_t valor =
            (leituraC & mascaras[i]) ? 1 : 0;

        atualizarBotao(i, valor, agora);
    }

    uint8_t reset =
        (leituraB & mascaras[4]) ? 1 : 0;

    return atualizarBotao(4, reset, agora);
}

// =====================================
// ADC - LDR A5
// =====================================

/** Ultima leitura do ADC do LDR (0..1023). */
uint16_t valorLDR = 0;
/** Leituras seguidas do LDR acima do limiar (filtro de confirmacao do acerto). */
uint8_t contadorDano = 0;
/** Instante da ultima leitura do LDR, em ms. */
uint32_t ultimaAmostraLDR = 0;

/**
 * @brief Configura o ADC para ler o LDR no canal 5 (A5).
 *
 * Referencia AVcc, prescaler 128 (125 kHz) e buffer digital do pino desligado.
 */
void configurarADC(void) {
    DDRC &= ~(1 << PC5);
    PORTC &= ~(1 << PC5);

    ADMUX = (1 << REFS0) | LDR_CANAL;

    ADCSRA = (1 << ADEN) |
             (1 << ADPS2) |
             (1 << ADPS1) |
             (1 << ADPS0);

    DIDR0 |= (1 << ADC5D);
}

/**
 * @brief Faz uma conversao do ADC e espera o resultado.
 *
 * @return Leitura de 10 bits (0 a 1023).
 */
uint16_t lerADC(void) {
    ADCSRA |= (1 << ADSC);

    while (ADCSRA & (1 << ADSC)) {
    }

    return ADC;
}

// Laser aumenta a leitura para 800-900+
// Nao exige rearme apos a protecao.

/**
 * @brief Le o LDR e informa se o laser foi detectado.
 *
 * So le a cada LDR_INTERVALO_MS. O acerto e confirmado quando LDR_CONFIRMACOES
 * leituras seguidas ficam em LDR_LIMIAR_DISPARO ou mais.
 *
 * @param agora Tempo atual, em ms.
 * @return 1 quando o acerto foi confirmado, 0 caso contrario.
 */
uint8_t verificarLDR(uint32_t agora) {
    if ((uint32_t)(agora - ultimaAmostraLDR) <
        LDR_INTERVALO_MS) {
        return 0;
    }

    ultimaAmostraLDR = agora;
    valorLDR = lerADC();

    if (valorLDR >= LDR_LIMIAR_DISPARO) {
        if (contadorDano < LDR_CONFIRMACOES) {
            contadorDano++;
        }

        if (contadorDano >= LDR_CONFIRMACOES) {
            contadorDano = 0;
            return 1;
        }
    } else {
        contadorDano = 0;
    }

    return 0;
}

// =====================================
// LEDS DAS VIDAS
// =====================================

/**
 * @brief Acende os LEDs de acordo com o numero de vidas.
 *
 * Grava so os tres bits dos LEDs no PORTD, sem alterar os demais pinos.
 */
void atualizarLEDs(void) {
    uint8_t mask =
        (1 << LED_V1) |
        (1 << LED_V2) |
        (1 << LED_V3);

    uint8_t saida = 0;

    if (vidas >= 1) saida |= (1 << LED_V1);
    if (vidas >= 2) saida |= (1 << LED_V2);
    if (vidas >= 3) saida |= (1 << LED_V3);

    PORTD = (PORTD & ~mask) | saida;
}

// =====================================
// CONTROLE MANUAL
// =====================================

/**
 * @brief Movimento manual: so anda se exatamente um botao de movimento estiver apertado.
 *
 * Com nenhum botao, ou com dois ou mais, os motores param.
 */
void controleManual(void) {
    uint8_t quantidade = 0;
    int8_t comando = -1;

    for (uint8_t i = 0; i < 4; i++) {
        if (estavel[i] == 0) {
            quantidade++;
            comando = i;
        }
    }

    if (quantidade != 1) {
        pararMotores();
        return;
    }

    switch (comando) {
        case 0:
            motores(VELOCIDADE, VELOCIDADE);
            break;

        case 1:
            motores(-VELOCIDADE, -VELOCIDADE);
            break;

        case 2:
            motores(-VELOCIDADE, VELOCIDADE);
            break;

        case 3:
            motores(VELOCIDADE, -VELOCIDADE);
            break;
    }
}

// =====================================
// DANO
// =====================================

/**
 * @brief Trata um acerto de laser: perde uma vida e inicia o giro automatico.
 *
 * So atua no estado ATIVO e com pelo menos uma vida.
 *
 * @param agora Tempo atual, em ms.
 */
void receberDano(uint32_t agora) {
    if (estado != ATIVO || vidas == 0) {
        return;
    }

    pararMotores();

    vidas--;
    atualizarLEDs();

    contadorDano = 0;
    inicioDano = agora;
    estado = ATINGIDO;

    // Giro automatico
    motores(VELOCIDADE, -VELOCIDADE);

    uartTexto(">>> DANO! ADC = ");
    uartNumero(valorLDR);
    uartTexto(" | VIDAS = ");
    uartNumero(vidas);
    uartTexto("\r\n");
}

// =====================================
// RESET
// =====================================

/**
 * @brief Devolve as 3 vidas e volta ao estado ATIVO (botao de reset).
 *
 * @param agora Tempo atual, em ms.
 */
void reiniciar(uint32_t agora) {
    pararMotores();

    vidas = 3;
    atualizarLEDs();

    contadorDano = 0;
    ultimaAmostraLDR = agora;

    estado = ATIVO;
    inicioDano = 0;

    uartLinha("RESET - 3 VIDAS");
}

// =====================================
// SETUP
// =====================================

/**
 * @brief Configura pinos, perifericos e interrupcoes. Executada uma vez, no inicio.
 *
 * Coloca os motores parados, acende os 3 LEDs, descarta a primeira conversao do ADC,
 * acende o laser e liga as interrupcoes.
 */
void setup(void) {
    cli();

    DDRD |=
        (1 << PWM_ESQ) |
        (1 << ESQ_1) |
        (1 << ESQ_2) |
        (1 << LED_V1) |
        (1 << LED_V2) |
        (1 << LED_V3);

    DDRB |=
        (1 << PWM_DIR) |
        (1 << DIR_1) |
        (1 << DIR_2) |
        (1 << LASER);

    PORTD &= ~(
        (1 << PWM_ESQ) |
        (1 << ESQ_1) |
        (1 << ESQ_2)
    );

    PORTB &= ~(
        (1 << PWM_DIR) |
        (1 << DIR_1) |
        (1 << DIR_2) |
        (1 << LASER)
    );

    configurarBotoes();
    configurarADC();
    configurarPWM();
    configurarTimer1();
    uartInit();

    pararMotores();

    vidas = 3;
    atualizarLEDs();

    // Primeira conversao ADC descartada
    lerADC();

    // Comeca com laser ligado
    contadorLaser = 0;
    laserLigado = 1;
    PORTB |= (1 << LASER);

    sei();

    uartLinha("============================");
    uartLinha("GIGANTES DE MDF");
    uartLinha("LASER: 1s ON / 1s OFF");
    uartLinha("LDR: ADC >= 800");
    uartLinha("GIRO: 500 ms (CALIBRADO)");
    uartLinha("PROTECAO: 5 SEGUNDOS");
    uartLinha("SERIAL: 115200");
    uartLinha("============================");
}

// =====================================
// LOOP PRINCIPAL
// =====================================

/**
 * @brief Laco principal, executado sem parar.
 *
 * Atualiza os botoes e age conforme o estado:
 *  - ATIVO: controle manual, leitura do LDR e mensagem de depuracao a cada
 *    SERIAL_INTERVALO_MS.
 *  - ATINGIDO: gira por TEMPO_GIRO_MS, para e espera TEMPO_PROTECAO_MS (sem ler o LDR);
 *    depois vai para ELIMINADO (sem vidas) ou volta a ATIVO.
 *  - ELIMINADO: motores parados ate o botao de reset ser apertado.
 */
void loop(void) {
    static uint32_t ultimoSerial = 0;

    uint32_t agora = agoraMs();

    uint8_t mudouReset = atualizarBotoes(agora);

    switch (estado) {

        case ATIVO: {
            controleManual();

            if (verificarLDR(agora)) {
                receberDano(agora);
                break;
            }

            if ((uint32_t)(agora - ultimoSerial) >=
                SERIAL_INTERVALO_MS) {

                ultimoSerial = agora;

                uartTexto("LDR = ");
                uartNumero(valorLDR);

                uartTexto(" | Confirmacoes = ");
                uartNumero(contadorDano);

                uartTexto(" | Vidas = ");
                uartNumero(vidas);

                uartTexto("\r\n");
            }

            break;
        }

        case ATINGIDO: {
            uint32_t dt = agora - inicioDano;

            if (dt < TEMPO_GIRO_MS) {
                motores(VELOCIDADE, -VELOCIDADE);
            } else {
                pararMotores();
            }

            // Sem leituras ADC durante a protecao
            if (dt >= TEMPO_PROTECAO_MS) {
                pararMotores();

                if (vidas == 0) {
                    estado = ELIMINADO;
                    uartLinha(
                        "ELIMINADO - PRESSIONE RESET"
                    );
                } else {
                    contadorDano = 0;
                    ultimaAmostraLDR = agora;

                    estado = ATIVO;
                    uartLinha("PROTECAO TERMINOU");
                }
            }

            break;
        }

        case ELIMINADO: {
            pararMotores();

            if (mudouReset && estavel[4] == 0) {
                reiniciar(agora);
            }

            break;
        }
    }
}

// =====================================
// MAIN
// =====================================

/**
 * @brief Ponto de entrada do programa.
 *
 * Executa setup() uma vez e depois repete loop() para sempre.
 *
 * @return Nunca retorna.
 */
int main(void) {
    setup();

    while (1) {
        loop();
    }

    return 0;
}
