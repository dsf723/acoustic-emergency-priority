`timescale 1ns / 1ps
// =============================================================================
// mic_array_i2s_rx 仿真
//  - 4 个 I2S 麦克风行为模型（每根数据线左右两个声道），在 SCK 下降沿后 40ns 输出数据
//  - 每个样本值由 (麦克风模型内部帧号, 通道号) 决定，检查 DUT 输出的值、通道号、
//    帧序号连续性、tlast 位置
//  - 阶段 1：tready=0（模拟 DMA 未启动）
//  - 阶段 2：tready 随机抖动，检查若干完整包，overrun 必须为 0
//  - 阶段 3：包中途长时间拉低 tready，overrun 必须置 1
// =============================================================================
module tb_mic_array_i2s_rx;

    localparam integer FPP = 8;     // 仿真用小包，便于检查 tlast

    reg clk = 0;
    reg rst_n = 0;
    always #10 clk = ~clk;          // 50 MHz

    wire        sck, ws;
    wire [3:0]  d;
    wire [31:0] tdata;
    wire        tvalid, tlast;
    reg         tready = 0;
    wire        overrun;
    wire [31:0] frame_cnt;

    mic_array_i2s_rx #(.SCK_HALF_DIV(8), .FRAMES_PER_PKT(FPP)) dut (
        .clk(clk), .rst_n(rst_n),
        .mic_sck(sck), .mic_ws(ws), .mic_d(d),
        .m_axis_tdata(tdata), .m_axis_tvalid(tvalid), .m_axis_tlast(tlast),
        .m_axis_tready(tready),
        .overrun(overrun), .frame_cnt(frame_cnt)
    );

    // 期望样本值：[23:16]=帧号，[15:12]=通道号，[11:0]=伪随机
    function [23:0] sample_val(input [7:0] fr, input [2:0] ch);
        sample_val = {fr, 1'b0, ch, fr[3:0] * 8'd37 + {ch, 5'd0} + 8'd5, 4'hA};
    endfunction

    // ---------------- 麦克风行为模型 ----------------
    reg [7:0]  mic_frame = 0;       // 模型帧号，每个 WS 下降沿（左声道开始）加 1
    reg        ws_prev   = 0;       // DUT 复位后处于左声道第 0 个 SCK
    integer    bitpos    = 0;
    reg [3:0]  d_reg     = 0;
    assign d = d_reg;

    integer k;
    always @(negedge sck) if (rst_n) begin     // 忽略复位时 X->0 的伪下降沿
        if (ws != ws_prev) begin
            bitpos = 0;
            if (!ws) mic_frame = mic_frame + 1;
        end else begin
            bitpos = bitpos + 1;
        end
        ws_prev = ws;
        for (k = 0; k < 4; k = k + 1) begin
            if (bitpos >= 1 && bitpos <= 24)
                d_reg[k] <= #40 sample_val(mic_frame, 2*k + ws) >> (24 - bitpos);
            else
                d_reg[k] <= #40 1'b0;
        end
    end

    // ---------------- 输出检查 ----------------
    integer errors = 0;
    integer beats  = 0;     // 包内字计数
    integer frames_ok = 0;
    integer pkts   = 0;
    reg [7:0] fr_cur;
    reg [3:0] seq_cur, seq_prev;
    reg       have_prev = 0;
    reg       check_en  = 0;

    always @(posedge clk) begin
        if (check_en && tvalid && tready) begin
            // 字格式
            if (tdata[26:24] !== beats[2:0]) begin
                errors = errors + 1;
                $display("ERR %0t: ch=%0d expect %0d", $time, tdata[26:24], beats % 8);
            end
            if (beats % 8 == 0) begin
                fr_cur  = tdata[23:16];
                seq_cur = tdata[31:28];
                // 包内帧序号必须连续；例外：第一个包的第 0 帧是 DMA 启动前滞留的旧帧
                if (have_prev && beats != 0 && !(pkts == 0 && beats == 8) &&
                    seq_cur !== seq_prev + 4'd1) begin
                    errors = errors + 1;
                    $display("ERR %0t: seq %0d after %0d", $time, seq_cur, seq_prev);
                end
            end else if (tdata[31:28] !== seq_cur) begin
                errors = errors + 1;
                $display("ERR %0t: seq changed inside frame", $time);
            end
            if (tdata[23:0] !== sample_val(fr_cur, beats % 8)) begin
                errors = errors + 1;
                $display("ERR %0t: ch%0d data %h expect %h", $time,
                         beats % 8, tdata[23:0], sample_val(fr_cur, beats % 8));
            end
            // tlast 位置
            if (tlast !== (beats == FPP*8 - 1)) begin
                errors = errors + 1;
                $display("ERR %0t: tlast=%b at beat %0d", $time, tlast, beats);
            end
            if (beats % 8 == 7) begin
                frames_ok = frames_ok + 1;
                seq_prev  = seq_cur;
                have_prev = 1;
            end
            if (tlast) begin
                beats = 0;
                pkts  = pkts + 1;
            end else begin
                beats = beats + 1;
            end
        end
    end

    // 阶段 2：随机反压
    reg rand_ready = 0;
    always @(posedge clk) begin
        if (rand_ready)
            tready <= ($random % 4) != 0;
    end

    localparam real FRAME_NS = 64.0 * 16 * 20;   // 一帧 20.48us

    initial begin
        #200 rst_n = 1;

        // 阶段 1：DMA 未启动
        #(FRAME_NS * 5);
        if (overrun) begin errors = errors + 1; $display("ERR: overrun while idle"); end

        // 阶段 2：开始接收，检查 3 个完整包
        @(posedge clk);
        check_en   = 1;
        rand_ready = 1;
        wait (pkts == 3);
        if (overrun) begin errors = errors + 1; $display("ERR: overrun with light backpressure"); end
        $display("phase2: %0d packets, %0d frames checked, frame_cnt=%0d", pkts, frames_ok, frame_cnt);

        // 阶段 3：包中途长时间停顿 -> overrun
        wait (beats == 20);
        @(posedge clk);
        rand_ready = 0;
        tready <= 0;
        #(FRAME_NS * 3);
        if (!overrun) begin errors = errors + 1; $display("ERR: overrun not set"); end

        if (frames_ok < FPP*3 || pkts < 3) begin
            errors = errors + 1;
            $display("ERR: too few frames checked");
        end
        if (errors == 0) $display("TEST PASSED");
        else             $display("TEST FAILED, %0d errors", errors);
        $finish;
    end

    initial begin
        #(FRAME_NS * 200);
        $display("TEST FAILED: timeout");
        $finish;
    end

endmodule
