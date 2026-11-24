// Pinos dos Motores (L293D / Ponte H)
const int velo1 = 3;  // Pino PWM
const int M11 = 2;  
const int M12 = 4;  

const int velo2 = 9;  // PWM do Motor 2
const int M21 = 8;  
const int M22 = 10;  

// Pino do Botão
const int pinoBotao = 6;

// Protótipos das Funções
void estadoParado();
void estadoFrente();
void estadoTras();

void setup() { 
    Serial.begin(9600); 

    pinMode(velo1, OUTPUT); 
    pinMode(M11, OUTPUT); 
    pinMode(M12, OUTPUT); 

    pinMode(velo2, OUTPUT); 
    pinMode(M21, OUTPUT); 
    pinMode(M22, OUTPUT); 
  
    // Configurado como INPUT puro
    pinMode(pinoBotao, INPUT);
  
    // Inicializa parado
    estadoParado();

    /* Implementação serial para complementar botoes
    Serial.println("--- Sistema Pronto ---");
    Serial.println("Comandos do Serial: 'f' (Frente), 't' (Tras), 'p' (Parar)");*/
} 

void loop() { 
    //Sem definição de botoes por enquanto apenas para teste, trabalherei com ifs para alternancia de funçoes 

    estadoFrente();
}

// ==========================================
// FUNÇÕES DE ESTADO DOS MOTORES
// ==========================================

void estadoParado() {
    analogWrite(velo1, 0);
    analogWrite(velo2, 0);
    
    digitalWrite(M11, LOW); 
    digitalWrite(M12, LOW); 
    digitalWrite(M21, LOW); 
    digitalWrite(M22, LOW); 

    Serial.println("Estado 0: Motores Parados");
}

void estadoFrente() {
    int velocidade = 200;

    analogWrite(velo1, velocidade);
    analogWrite(velo2, velocidade);

    digitalWrite(M11, HIGH); 
    digitalWrite(M12, LOW); 
    digitalWrite(M21, HIGH); 
    digitalWrite(M22, LOW); 

    Serial.println("Estado 1: Girando para Frente");
}

void estadoTras() {
    int velocidade = 200;

    analogWrite(velo1, velocidade);
    analogWrite(velo2, velocidade);

    digitalWrite(M11, LOW); 
    digitalWrite(M12, HIGH); 
    digitalWrite(M21, LOW); 
    digitalWrite(M22, HIGH); 

    Serial.println("Estado 2: Girando para Tras");
}