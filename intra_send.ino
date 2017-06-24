#include <MsTimer2.h>
#include <SPI.h>
#include <SD.h>

#define PORT_DETECT 2               // SDカード検知LED(OUT)
#define PORT_CONNECT 3              // PC通信確立LED(OUT)
#define PORT_RESET 4                // 予備入力(IN)
#define PORT_CD 5                   // SDカード検知接点(IN)
#define PORT_BR1 6                  // ボーレート選択1(IN)
#define PORT_BR2 7                  // ボーレート選択2(IN)
#define PORT_SW3 8                  // 予備入力(IN)
#define PORT_SW4 9                  // 予備入力(IN)

#define SS 10                       // SDカードセレクト

#define CMD_ACCEPT          48     // 接続要求
#define CMD_END             49     // 終了要求
#define CMD_PING            50     // PING
#define CMD_SD_CONFIG       51     // SDコンフィグ送信
#define CMD_SD_DISCONNECT   52     // SD取り外し検出
#define CMD_FILE_SEND       53     // ファイル送信フラグ
#define CMD_FILE_NACK       54     // ファイル受信失敗
#define CMD_FILE_ACK        55     // ファイル受信応答

#define CMD_NOP 0xFF                // ノンオペレーション

#define BUFFER_SIZE 63             // シリアル受信バッファサイズ



// ---------------------------------------------------------------------------------------------


uint32_t getUsingsize(File dir, uint32_t s);
void SD_dispose();

// ---------------------------------------------------------------------------------------------

// SD
Sd2Card card;
File root;
uint8_t sd_type;            // 1:SD1 | 2:SD2 | 3:SDHC
uint32_t volumesize;        // SD容量
uint32_t usedsize;          // 使用済み容量
volatile bool sd_flag = false;       // SD認識フラグ
volatile bool sd_standby = false;    // SDスタンバイ完了フラグ
volatile bool sd_disconnect = false; // SD取り外しフラグ

volatile bool pc_connect = false;    // PC接続フラグ

volatile int led_pc_blink = 0;      // 0以上であれば点滅
volatile int led_sd_blink = 0;      // 0以上であれば点滅

// ----------------------------------------------------------------------------------------------
// タイマー割り込み
// ----------------------------------------------------------------------------------------------
void msTimer_interrupt(){
  // LED表示 PC接続
  if (pc_connect == true){
    if (led_pc_blink > 0){
      // 点滅させる
      if (led_pc_blink % 2 == 1)
        digitalWrite(PORT_CONNECT, HIGH);
      else
        digitalWrite(PORT_CONNECT, LOW);
      led_pc_blink--;
    }else
      digitalWrite(PORT_CONNECT, LOW);      
    
  }else
    digitalWrite(PORT_CONNECT, HIGH);
}



// ----------------------------------------------------------------------------------------------
// セットアップ
// ----------------------------------------------------------------------------------------------
void setup() {
  int i;
  // ポート初期化
  pinMode(PORT_DETECT, OUTPUT);
  pinMode(PORT_CONNECT, OUTPUT);
  pinMode(PORT_CD, INPUT);
  pinMode(PORT_BR1, INPUT);
  pinMode(PORT_BR2, INPUT);
  pinMode(PORT_SW3, INPUT);
  pinMode(PORT_SW4, INPUT);
  pinMode(13, OUTPUT);
  pinMode(SS, OUTPUT);

  // イニシャルブリンク
  for (i = 0; i < 7; i++){
    digitalWrite(PORT_DETECT, HIGH);
    digitalWrite(PORT_CONNECT, HIGH);
    delay(40);
    digitalWrite(PORT_DETECT, LOW);
    digitalWrite(PORT_CONNECT, LOW);
    delay(20);
  }
  digitalWrite(PORT_DETECT, HIGH);
  // シリアル初期化
  Serial.begin(getBaudrate());
  //Serial.print(getBaudrate());
  // MsTimer2初期化
  MsTimer2::set(20, msTimer_interrupt);
  MsTimer2::start();
  
}

// ----------------------------------------------------------------------------------------------
// メインループ
// ----------------------------------------------------------------------------------------------
void loop() {
  int i;
  byte rData;       // データ受信
  int timout = 0;   // 

  // PC通信処理 --------------------------------------------------------------------------------
  if (pc_connect == false) {
    // 未接続
    if (Serial.available() > 0) {
      rData = Serial.read();  // 1バイト読み込み
      if (rData == CMD_ACCEPT) {
        // 接続要求ならACCEPTを返す
        pc_connect = true;
        spWrite(CMD_ACCEPT);
      }else{
        spWrite(CMD_END);
      }
    }
      
  } else {
    // 接続済み
    if (Serial.available() > 0) {
      rData = Serial.read();  // 1バイト読み込み
      int i;
      switch (rData) {
        case CMD_END:
          // 終了する
          pc_connect = false;
          SD_dispose();
          break;
          
        case CMD_PING:
          spWrite(CMD_PING); // PINGを返す
          break;

        case CMD_FILE_SEND:
          //PCからファイルを受信しSDに書き込む
          file_ReciveWrite();
          break;
         
        default:
          //spWrite(CMD_NOP);
          break;
      }
    }

    // SD状態の送信
    if (sd_standby) {
      // SDコンフィグをPCに送信
      spWrite(CMD_SD_CONFIG);
      waitACK();        // PCからのACKを待つ
     
      // SDフォーマット送信
      spWrite(sd_type);
      waitACK();
      // SD容量
      spWrite((volumesize >> 24) & 0xFF);
      spWrite((volumesize >> 16) & 0xFF);
      spWrite((volumesize >> 8) & 0xFF);
      spWrite(volumesize & 0xFF);
      waitACK();
      // SD使用容量
      spWrite((usedsize >> 24) & 0xFF);
      spWrite((usedsize >> 16) & 0xFF);
      spWrite((usedsize >> 8) & 0xFF);
      spWrite(usedsize & 0xFF);
      waitACK();
      sd_standby = false;
    } else if (sd_disconnect) {
      // SDが抜かれたことをPCに送信
      spWrite(CMD_SD_DISCONNECT);
      sd_disconnect = false;
    }
  }

  // SD処理 -----------------------------------------------------------------------------------
  if (sd_flag == true) {
    // SD接続中
    // SDを抜かれたらリセット
    if (digitalRead(PORT_CD) == HIGH) {
      SD_dispose();
      digitalWrite(PORT_DETECT, HIGH);
    }

  } else if (sd_flag == false) {
    // SD未接続
    if (digitalRead(PORT_CD) == LOW) {
      digitalWrite(PORT_DETECT, LOW);
      // SD検知
      SD.begin();
      // カードタイプ
      sd_type = SD.card.type();
      // 合計容量
      volumesize = SD.card.cardSize() * 512;        // SD sectors(512) count
      // 使用容量
      // ファイルを順番に開きサイズだけ取得する
      root = SD.open("/");
      usedsize = getUsingsize(root, 0);
      root.close();
      // モード移行
      sd_flag = true;
      sd_standby = true;       // PCに一度だけ情報送信
      delay(100);
    }
  }
}



// ----------------------------------------------------------------------------------------------
// ファイル受信してSDに書き込む
// ----------------------------------------------------------------------------------------------
void file_ReciveWrite(){
  unsigned long i, j, k;             // いろいろアイジェイ
  byte rec = 0;             // 受信データバイト
  char fname[64];           // ファイル名
  unsigned long fsize = 0;  // ファイルサイズ

  // データ送信コマンドへの応答
  spWrite(CMD_FILE_ACK);
  
  // ファイル名受信---------------------------------------
  i = 0;
  while(rec != 0x0A){
    if (Serial.available() > 0){
      rec = Serial.read();
      // 文字コードなら使用する
      if (rec > 0x20 && rec < 0x7A)
        fname[i++] = rec;    
    }
  }
  fname[i] = '\0';
  spWrite(CMD_FILE_ACK);    // PCへ応答

  // ファイルサイズ受信----------------------------------
  waitData(4);
  fsize = (unsigned long)Serial.read() << 24;
  fsize += (unsigned long)Serial.read() << 16;
  fsize += (unsigned long)Serial.read() << 8;
  fsize += (unsigned long)Serial.read();
  spWrite(CMD_FILE_ACK);    // PCへ応答
  
  // ファイルが存在する場合削除--------------------------
  if (SD.exists(fname)){
    // 存在するため削除
    if (SD.remove(fname) == false){
      // ファイルを削除できないためNACKを返し
      // 当該ファイルの受信処理を終了する
      log_out(fsize, fname, "file delete failed.");
      spWrite(CMD_FILE_NACK);    
      return;
    }
  }
//  log_out(fsize, fname, "*************");
  // データを受信してバイナリ書き出し--------------------
//  File f = SD.open("rrr.txt", FILE_WRITE);    // ファイルを生成(テストファイル名)

  // ファイルを生成
  File f = SD.open(fname, FILE_WRITE);    
  // 処理結果検証
  if (f == false){
    // ファイルを生成できないためNACKを返し
    // 当該ファイルの受信処理を終了する
    f.close();
    log_out(fsize, fname, "file open failed.");
//    spWrite(CMD_FILE_NACK);    
    return;
  }
  
  // データ受信ループ
  for(i = 0; i < fsize - (fsize % BUFFER_SIZE); i += BUFFER_SIZE){
    // 点滅
    digitalWrite(PORT_DETECT, !digitalRead(PORT_DETECT));
    
    // 書き出し
    for(j = 0; j < BUFFER_SIZE; j++){
      if(waitData(1) == false){
          f.close();
          log_out(fsize, fname, "time out for 64 bytes data.");
          return;
      }
      rec = Serial.read();
      f.write(rec); 
    }
    spWrite(CMD_FILE_ACK);    // PCへ応答
  }



  // 最後のデータを受信
  
  for(j = 0; j < fsize % BUFFER_SIZE; j++){
      // 点滅
      digitalWrite(PORT_DETECT, !digitalRead(PORT_DETECT));
      if(waitData(1) == false){
        f.close();
        log_out(fsize, fname, "time out for last bytes data.");
        return;
      }
      rec = Serial.read();
      f.write(rec); 
  }
  spWrite(CMD_FILE_ACK);    // PCへ応答

   // ファイルを閉じる
  f.flush();
  f.close();
  log_out(fsize, fname, "succeed.");
  delay(100);

}




// ----------------------------------------------------------------------------------------------
// SDの使用容量を取得
// 全ファイルを開きsizeを取得する再帰関数
//   dir    : 今開いているファイル
//   s      : ファイルサイズ
//   return : ファイルサイズの合計
// ----------------------------------------------------------------------------------------------
uint32_t getUsingsize(File dir, uint32_t s) {
  uint32_t ss;
  File entry =  dir.openNextFile();
  if (! entry) {
    return 0;
  }
  if (entry.isDirectory()) {
    return getUsingsize(entry, s);
  } else {
    ss = entry.size() + getUsingsize(entry, s);
    entry.close();
    return ss;
  }
}

// ----------------------------------------------------------------------------------------------
// SDを終了する
// ----------------------------------------------------------------------------------------------
void SD_dispose() {
  SPI.end();
  volumesize = 0;
  usedsize = 0;
  sd_flag = false;
  sd_standby = false;
  sd_disconnect = true;
  // Serial.print("exit...\n\n");
}


// ----------------------------------------------------------------------------------------------
// SW1,SW2よりボーレートを判定する
//  00:4800
//  01:9600
//  10:57600
//  11:115200
// ----------------------------------------------------------------------------------------------
long getBaudrate(){
  if (digitalRead(PORT_BR2) == LOW){
    if (digitalRead(PORT_BR1) == LOW){
      return 4800;
    }else{
      return 9600;
    }
  }else{
    if (digitalRead(PORT_BR1) == HIGH){
      return 115200;
    }else{
      return 57600;
    }
  }
  return 9600;
}

// ----------------------------------------------------------------------------------------------
// PCからのデータを待つ
//  return  : true->データ到達, false->タイムアウトした
// ----------------------------------------------------------------------------------------------
bool waitACK(){
  int timeout = 0;
  while (Serial.available() == 0){
    delay(1);
    if (timeout++ > 5000){
      return false;
    }
  }
  return true;
}

// ----------------------------------------------------------------------------------------------
// PCからの受信データが指定数になるまでウェイト
//  s       : データ到達数
//  return  : true-データ到達, false-タイムアウトした
// ----------------------------------------------------------------------------------------------
bool waitData(int s){
  int timeout = 0;
  while (Serial.available() < s){
    delay(10);
    if (timeout++ > 500){
      return false;
    }
  }
  return true;
}

// ----------------------------------------------------------------------------------------------
// PCにデータを1バイト送信する()
//  d     : 送信データバイト
// ----------------------------------------------------------------------------------------------
void spWrite(byte d){
  Serial.write(d);
  led_pc_blink += 1;
}

// ----------------------------------------------------------------------------------------------
// ログを出力する(DEBUG.TXTを生成 or 追記)
// SD.beginを実行した後でFileインスタンスが存在しない状態で使用する
//  fsize   : ファイルサイズ
//  fname   : ファイル名
//  msg     : 備考
// ----------------------------------------------------------------------------------------------
void log_out(int fsize, char *fname, char *msg){
  File f = SD.open("debug.log", FILE_WRITE);
  f.print("*fsize:");
  f.print(fsize, DEC);
  f.print("\n");
  f.print("fname:");
  f.print(fname);
  f.print("\n");
  f.print("msg:");
  f.print(msg);
  f.print("\n");
  f.flush();
  f.close();
}








