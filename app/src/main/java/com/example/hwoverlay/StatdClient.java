package com.example.hwoverlay;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

/** Gửi lệnh một dòng tới daemon statd (127.0.0.1:27183). Gọi trên luồng nền. */
final class StatdClient {
    private static final int PORT = 27183;

    private StatdClient() {}

    /** Trả về dòng trả lời ("ok ..." / "err ..."), hoặc null nếu statd không chạy. */
    static String request(String line) {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress("127.0.0.1", PORT), 500);
            s.setSoTimeout(3000);
            OutputStream out = s.getOutputStream();
            out.write((line + "\n").getBytes(StandardCharsets.UTF_8));
            out.flush();
            BufferedReader in = new BufferedReader(
                    new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8));
            // statd cũng phát số liệu định kỳ trên cùng kết nối: bỏ qua cho tới dòng trả lời.
            for (String r; (r = in.readLine()) != null; ) {
                if (r.startsWith("ok") || r.startsWith("err")) return r;
            }
        } catch (Exception ignored) {
            // statd chưa chạy hoặc hết thời gian chờ
        }
        return null;
    }
}
