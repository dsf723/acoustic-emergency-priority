`timescale 1ns / 1ps
// =============================================================================
// hdmi_fb 仿真
//  1. AXI4-Lite 写入前 3 行测试图案，读回校验，读状态寄存器
//  2. 检查有效区每个像素的 RGB（调色板）与写入的颜色编号一致
//  3. 用参考解码器把 3 路 TMDS 符号解码回 RGB，与编码前比较；检查直流平衡
//  4. 在串行输出端（DDR 采样）恢复 10bit 符号，确认能在固定位对齐下找到控制符号，
//     验证 OSERDES 的位顺序（低位先发）
// =============================================================================
module tb_hdmi_fb;

    reg aclk = 0, aresetn = 0, clk50 = 0;
    always #10 aclk = ~aclk;           // AXI 50MHz
    always #10.3 clk50 = ~clk50;       // 视频晶振 50MHz（略不同，模拟异步）

    reg  [17:0] awaddr = 0, araddr = 0;
    reg         awvalid = 0, wvalid = 0, bready = 0, arvalid = 0, rready = 0;
    reg  [31:0] wdata = 0;
    wire        awready, wready, bvalid, arready, rvalid;
    wire [1:0]  bresp, rresp;
    wire [31:0] rdata;
    wire        tclk_p, tclk_n;
    wire [2:0]  td_p, td_n;

    hdmi_fb dut (
        .s_axi_aclk(aclk), .s_axi_aresetn(aresetn),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'd0), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(4'hF), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arprot(3'd0), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready),
        .clk_in(clk50), .hdmi_hpd(1'b1),
        .tmds_ck_p(tclk_p), .tmds_ck_n(tclk_n), .tmds_data_p(td_p), .tmds_data_n(td_n)
    );

    integer errors = 0, pix_err = 0, tmds_err = 0;

    task axi_write(input [17:0] a, input [31:0] d);
        begin
            @(posedge aclk);
            awaddr <= a; awvalid <= 1; wdata <= d; wvalid <= 1; bready <= 1;
            fork
                begin @(posedge aclk); while (!(awvalid && awready)) @(posedge aclk); awvalid <= 0; end
                begin @(posedge aclk); while (!(wvalid && wready)) @(posedge aclk); wvalid <= 0; end
            join
            while (!bvalid) @(posedge aclk);
            @(posedge aclk); bready <= 0;
        end
    endtask

    task axi_read(input [17:0] a, output [31:0] d);
        begin
            @(posedge aclk);
            araddr <= a; arvalid <= 1; rready <= 1;
            @(posedge aclk); while (!(arvalid && arready)) @(posedge aclk);
            arvalid <= 0;
            while (!rvalid) @(posedge aclk);
            d = rdata;
            @(posedge aclk); rready <= 0;
        end
    endtask

    // 期望的颜色编号：第 0 行 x%16，第 1 行 4（红），第 2 行 (x/8)%16，其余 0
    function [3:0] expect_idx(input integer x, input integer y);
        expect_idx = (y == 0) ? (x % 16) : (y == 1) ? 4'd4 : (y == 2) ? ((x / 8) % 16) : 4'd0;
    endfunction

    function [23:0] palette(input [3:0] i);
        case (i)
        4'd0: palette = 24'h0A0E1A;  4'd1: palette = 24'hFFFFFF;  4'd2: palette = 24'h3A4256;
        4'd3: palette = 24'h8A93A8;  4'd4: palette = 24'hFF3030;  4'd5: palette = 24'h30E060;
        4'd6: palette = 24'h3080FF;  4'd7: palette = 24'hFFD030;  4'd8: palette = 24'h30E0E0;
        4'd9: palette = 24'hE040E0;  4'd10: palette = 24'hFF8C20; 4'd11: palette = 24'h142040;
        4'd12: palette = 24'h104020; 4'd13: palette = 24'h501010; 4'd14: palette = 24'hC8CED8;
        default: palette = 24'h000000;
        endcase
    endfunction

    function [7:0] tmds_decode(input [9:0] q);
        reg [7:0] dd;
        integer k;
        begin
            dd = q[9] ? ~q[7:0] : q[7:0];
            tmds_decode[0] = dd[0];
            for (k = 1; k < 8; k = k + 1)
                tmds_decode[k] = q[8] ? (dd[k] ^ dd[k-1]) : ~(dd[k] ^ dd[k-1]);
        end
    endfunction

    // ---------------- 像素检查（编码前） ----------------
    integer px = 0, py = 0, pix_checked = 0, frames_seen = 0;
    reg de_prev = 0, vs_prev = 1;
    always @(posedge dut.pix_clk) begin
        if (!dut.pix_rst) begin
            if (dut.de_q) begin
                // 只检查第二帧（第一帧扫描时测试图案还在写入）
                if (frames_seen >= 1) begin
                    if (dut.rgb_q !== palette(expect_idx(px, py))) begin
                        if (pix_err < 10)
                            $display("ERR pixel (%0d,%0d) rgb %h expect %h", px, py, dut.rgb_q, palette(expect_idx(px, py)));
                        pix_err = pix_err + 1;
                        errors = errors + 1;
                    end
                    pix_checked = pix_checked + 1;
                end
                px = px + 1;
            end
            if (de_prev && !dut.de_q) begin
                px = 0;
                py = py + 1;
            end
            if (!vs_prev && dut.vs_q) begin       // 场同步结束：下一帧从第 0 行开始
                py = 0;
                frames_seen = frames_seen + 1;
            end
            de_prev = dut.de_q;
            vs_prev = dut.vs_q;
        end
    end

    // ---------------- TMDS 解码检查（编码器延迟 1 拍） ----------------
    reg [23:0] rgb_d1;
    reg        de_d1 = 0;
    integer    sym_checked = 0, disp0 = 0, disp_max = 0;
    integer    k;
    always @(posedge dut.pix_clk) begin
        if (!dut.pix_rst) begin
            if (de_d1) begin
                if ({tmds_decode(dut.sym2), tmds_decode(dut.sym1), tmds_decode(dut.sym0)} !== rgb_d1) begin
                    if (tmds_err < 10)
                        $display("ERR tmds decode %h expect %h", {tmds_decode(dut.sym2), tmds_decode(dut.sym1), tmds_decode(dut.sym0)}, rgb_d1);
                    tmds_err = tmds_err + 1;
                    errors = errors + 1;
                end
                sym_checked = sym_checked + 1;
                for (k = 0; k < 10; k = k + 1)
                    disp0 = disp0 + (dut.sym0[k] ? 1 : -1);
                if (disp0 > disp_max) disp_max = disp0;
                if (-disp0 > disp_max) disp_max = -disp0;
            end else begin
                disp0 = 0;
            end
            rgb_d1 <= dut.rgb_q;
            de_d1  <= dut.de_q;
        end
    end

    // ---------------- 串行端恢复符号（蓝通道），统计各位对齐下控制符号出现次数 ----------------
    reg [9:0] sr = 0;
    integer   bitpos = 0;
    integer   hits [0:9];
    integer   j;
    initial for (j = 0; j < 10; j = j + 1) hits[j] = 0;
    task sample_bit;
        begin
            sr = {td_p[0], sr[9:1]};                    // 先发的位最终落在 sr[0]
            if (sr == 10'b1010101011 || sr == 10'b0101010100)
                hits[bitpos % 10] = hits[bitpos % 10] + 1;
            bitpos = bitpos + 1;
        end
    endtask
    always @(posedge dut.ser_clk) if (!dut.pix_rst) #0.5 sample_bit;
    always @(negedge dut.ser_clk) if (!dut.pix_rst) #0.5 sample_bit;

    reg [31:0] rd;
    integer x, y, best, besth, total;
    initial begin
        #200 aresetn = 1;
        // 写测试图案（前 3 行）
        for (y = 0; y < 3; y = y + 1)
            for (x = 0; x < 80; x = x + 1) begin : wr
                reg [31:0] w;
                integer n;
                w = 0;
                for (n = 0; n < 8; n = n + 1)
                    w = w | (expect_idx(x * 8 + n, y) << (n * 4));
                axi_write((y * 80 + x) * 4, w);
            end
        // 读回校验
        axi_read((1 * 80 + 5) * 4, rd);
        if (rd !== 32'h44444444) begin errors = errors + 1; $display("ERR readback %h", rd); end
        axi_read((0 * 80 + 1) * 4, rd);
        if (rd !== 32'hFEDCBA98) begin errors = errors + 1; $display("ERR readback %h", rd); end
        // 越界写不应影响帧缓冲
        axi_write(18'h3FFF0, 32'hDEADBEEF);
        axi_read(18'h3FFF0, rd);
        $display("status reg = %h (bit0 = HPD)", rd);
        if (rd[0] !== 1'b1) begin errors = errors + 1; $display("ERR HPD bit"); end

        // 跑完整一帧后再检查 4 行（第二帧开头），覆盖场同步与跨帧
        wait (frames_seen == 1 && py == 4);
        $display("pixels checked %0d (errors %0d), tmds symbols checked %0d (errors %0d), max running disparity %0d",
                 pix_checked, pix_err, sym_checked, tmds_err, disp_max);
        best = 0; besth = 0; total = 0;
        for (j = 0; j < 10; j = j + 1) begin
            total = total + hits[j];
            if (hits[j] > besth) begin besth = hits[j]; best = j; end
        end
        $display("serial control-symbol hits: best offset %0d = %0d of %0d", best, besth, total);
        if (besth < 1000 || besth * 100 < total * 99) begin
            errors = errors + 1;
            $display("ERR serial bit order / alignment");
        end
        if (disp_max > 20) begin errors = errors + 1; $display("ERR DC balance"); end
        if (pix_checked < 640 * 4) begin errors = errors + 1; $display("ERR too few pixels"); end
        if (errors == 0) $display("TEST PASSED");
        else             $display("TEST FAILED, %0d errors", errors);
        $finish;
    end

    initial begin
        #40_000_000;
        $display("TEST FAILED: timeout");
        $finish;
    end
endmodule
