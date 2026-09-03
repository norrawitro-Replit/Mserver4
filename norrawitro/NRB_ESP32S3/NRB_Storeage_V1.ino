// ประกาศตัวแปรร่วม ใช้ได้ทุกฟังก์ชัน
String recvText = "";
String question2 = "";
bool question3 = 1;  // เปิดการทำงานเริ่มต้น (แก้ไขชนิดตัวแปร)
String answer2 = "";
bool answer3 = 1;  // เปิดการทำงานเริ่มต้น (แก้ไขชนิดตัวแปร)
String textshow2 = "";
bool textshow3 = 1;  // เปิดการทำงานเริ่มต้น (แก้ไขชนิดตัวแปร)

void setup() {
  Serial.begin(115200);
  while (!Serial)
    ;

  Serial.println("=====================================");
  Serial.println("     เปิดพอร์ทซีเรียลสำเร็จแล้ว     ");
  Serial.println("=====================================");
  Serial.print("ความเร็ว: ");
  Serial.print(115200);
  Serial.println(" บอด");
  Serial.println("ระบบพร้อมใช้งาน...\n");
  delay(1000);
}

void loop() {
  serialinput();  // 1. รับข้อมูลก่อน
  question1();    // 2. ย้ายค่าไปเก็บที่ question2
  answer1();      // 3. ตรวจสอบและสร้างคำตอบ
  textshow1();    // 4. แสดงผลลัพธ์
  delay(1000);
}

void serialinput() {
  if (Serial.available() > 0) {
    recvText = Serial.readStringUntil('\n');  // เก็บค่าลงตัวแปรร่วม
    recvText.trim();                          // ตัดช่องว่าง/อักขระพิเศษที่ติดมา
    Serial.print("New massage = ");
    Serial.println(recvText);
  }
}

void question1() {
  if (question3 == 1) {
    question2 = recvText;  // ย้ายค่าที่รับได้ไปเก็บ
    recvText = "";         // ล้างค่าเก่าทิ้ง
  }
}

void textshow1() {
  if (textshow3 == 1 && textshow2 != "") {  // แสดงเมื่อมีข้อความให้แสดง
    Serial.print("Answer = ");
    Serial.println(textshow2);
    textshow2 = "";  // ล้างค่าหลังแสดง
    // textshow3 = 0; // ถ้าต้องการปิดการแสดงถาวร ค่อยเปิดใช้บรรทัดนี้
  }
}

void answer1() {
  if (answer3 == 1 && question2 != "") {  // ทำงานเมื่อมีข้อมูลให้ตรวจสอบ
    if (question2 == "1") {
      answer2 = "1111";
    } else {
      answer2 = "No  data";
    }
    textshow2 = answer2;  // ส่งคำตอบไปยังตัวแปรสำหรับแสดงผล
    question2 = "";       // ล้างค่าเก่าทิ้ง
  }
}