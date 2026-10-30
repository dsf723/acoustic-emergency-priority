`timescale 1ns / 1ps
// =============================================================================
// HDMI（DVI 编码）帧缓冲显示：640x480@60Hz，16 色调色板，每像素 4bit
//
// 帧缓冲：38400 个 32bit 字放在双口 BRAM 中，每字 8 个像素，低 4bit 为最左边的像素；
//         第 y 行第 x 个像素在字地址 y*80 + x/8 的第 (x%8)*4 位起。
// AXI4-Lite 从机（s_axi_aclk 时钟域，32bit，只支持整字写，WSTRB 忽略）：
//   0x00000 ~ 0x257FC  帧缓冲（字节地址 = 字地址 * 4）
//   0x3FFF0            只读状态：bit0 = HPD（显示器已连接），bit31:8 = 帧计数
// 视频时钟：clk_in（板上 50MHz 晶振）经 MMCM 生成像素时钟约 25.16MHz 与 5 倍串行时钟，
//           与 AXI 时钟异步，只通过双口 BRAM 和同步器交互。
// 输出：TMDS_33 差分（OSERDESE2 10:1 DDR 串行化），不发送 HDMI 数据岛（DVI 模式），
//       HDMI 显示器均可显示。
// =============================================================================
module hdmi_fb (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 s_axi_aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axi, ASSOCIATED_RESET s_axi_aresetn" *)
    input  wire        s_axi_aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 s_axi_aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire        s_axi_aresetn,
    input  wire [17:0] s_axi_awaddr,
    input  wire [2:0]  s_axi_awprot,
    input  wire        s_axi_awvalid,
    output reg         s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output reg         s_axi_wready,
    output reg  [1:0]  s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready,
    input  wire [17:0] s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output reg         s_axi_arready,
    output reg  [31:0] s_axi_rdata,
    output reg  [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready,

    input  wire        clk_in,          // 50MHz 板载晶振
    input  wire        hdmi_hpd,        // 热插拔检测
    output wire        tmds_ck_p,
    output wire        tmds_ck_n,
    output wire [2:0]  tmds_data_p,
    output wire [2:0]  tmds_data_n
);

    localparam integer FB_WORDS = 38400;

    // -------------------------------------------------------------------------
    // 帧缓冲 BRAM（真双口：A 口 AXI 读写，B 口视频读取）
    // -------------------------------------------------------------------------
    (* ram_style = "block" *) reg [31:0] fb [0:FB_WORDS-1];
    integer fb_i;
    initial for (fb_i = 0; fb_i < FB_WORDS; fb_i = fb_i + 1) fb[fb_i] = 32'd0;   // 上电黑屏

    reg         a_we;
    reg  [15:0] a_addr;
    reg  [31:0] a_wdata;
    reg  [31:0] a_rdata;
    always @(posedge s_axi_aclk) begin
        if (a_we)
            fb[a_addr] <= a_wdata;
        a_rdata <= fb[a_addr];
    end

    wire        pix_clk;
    reg  [15:0] b_addr;
    reg  [31:0] b_rdata;
    reg  [31:0] b_rdata_q;
    always @(posedge pix_clk) begin
        b_rdata   <= fb[b_addr];
        b_rdata_q <= b_rdata;           // 输出寄存器，读延迟 2 拍
    end

    // -------------------------------------------------------------------------
    // AXI4-Lite 从机
    // -------------------------------------------------------------------------
    reg  [31:0] status_sync1, status_sync2;
    reg  [23:0] frame_cnt_pix;
    reg  [2:0]  hpd_sync;
    reg  [17:0] waddr;
    reg         have_aw, have_w;
    reg  [1:0]  rd_state;
    reg  [17:0] raddr;

    wire [31:0] status_pix = {frame_cnt_pix, 7'd0, hpd_sync[2]};

    always @(posedge s_axi_aclk) begin
        status_sync1 <= status_pix;     // 低速状态，允许个别位跨拍不一致
        status_sync2 <= status_sync1;
    end

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
            s_axi_bvalid  <= 1'b0;
            s_axi_bresp   <= 2'b00;
            have_aw       <= 1'b0;
            have_w        <= 1'b0;
            a_we          <= 1'b0;
            a_addr        <= 16'd0;
            a_wdata       <= 32'd0;
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rresp   <= 2'b00;
            s_axi_rdata   <= 32'd0;
            rd_state      <= 2'd0;
            raddr         <= 18'd0;
            waddr         <= 18'd0;
        end else begin
            a_we <= 1'b0;

            // 写：地址与数据各自握手，收齐后写入 BRAM 并回应
            s_axi_awready <= !have_aw && !s_axi_bvalid && rd_state == 2'd0;
            s_axi_wready  <= !have_w && !s_axi_bvalid && rd_state == 2'd0;
            if (s_axi_awvalid && s_axi_awready) begin
                have_aw <= 1'b1;
                waddr   <= s_axi_awaddr;
                s_axi_awready <= 1'b0;
            end
            if (s_axi_wvalid && s_axi_wready) begin
                have_w  <= 1'b1;
                a_wdata <= s_axi_wdata;
                s_axi_wready <= 1'b0;
            end
            if (have_aw && have_w && !s_axi_bvalid) begin
                if (waddr[17:2] < FB_WORDS) begin
                    a_we   <= 1'b1;
                    a_addr <= waddr[17:2];
                end
                have_aw      <= 1'b0;
                have_w       <= 1'b0;
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= 2'b00;
            end
            if (s_axi_bvalid && s_axi_bready)
                s_axi_bvalid <= 1'b0;

            // 读：0 空闲，1 等 BRAM，2 输出
            case (rd_state)
            2'd0: begin
                s_axi_arready <= !have_aw && !have_w && !s_axi_bvalid && !a_we;
                if (s_axi_arvalid && s_axi_arready) begin
                    s_axi_arready <= 1'b0;
                    raddr    <= s_axi_araddr;
                    a_addr   <= s_axi_araddr[17:2];
                    rd_state <= 2'd1;
                end
            end
            2'd1: rd_state <= 2'd3;     // BRAM 读延迟
            2'd3: begin
                s_axi_rdata  <= (raddr[17:2] == 16'hFFFC) ? status_sync2 :
                                (raddr[17:2] < FB_WORDS)  ? a_rdata : 32'd0;
                s_axi_rresp  <= 2'b00;
                s_axi_rvalid <= 1'b1;
                rd_state     <= 2'd2;
            end
            2'd2: if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
                rd_state     <= 2'd0;
            end
            endcase
        end
    end

    // -------------------------------------------------------------------------
    // 时钟：50MHz -> VCO 1006.25MHz（M=20.125）-> 像素 /40 = 25.156MHz，串行 /8 = 125.78MHz
    // -------------------------------------------------------------------------
    wire clk_fb, clk_fb_buf, pix_clk_mmcm, ser_clk_mmcm, ser_clk, mmcm_locked;
    MMCME2_BASE #(
        .CLKIN1_PERIOD    (20.0),
        .DIVCLK_DIVIDE    (1),
        .CLKFBOUT_MULT_F  (20.125),
        .CLKOUT0_DIVIDE_F (40.0),
        .CLKOUT1_DIVIDE   (8)
    ) u_mmcm (
        .CLKIN1   (clk_in),
        .CLKFBIN  (clk_fb_buf),
        .CLKFBOUT (clk_fb),
        .CLKOUT0  (pix_clk_mmcm),
        .CLKOUT1  (ser_clk_mmcm),
        .LOCKED   (mmcm_locked),
        .PWRDWN   (1'b0),
        .RST      (1'b0),
        .CLKFBOUTB(), .CLKOUT0B(), .CLKOUT1B(), .CLKOUT2(), .CLKOUT2B(),
        .CLKOUT3(), .CLKOUT3B(), .CLKOUT4(), .CLKOUT5(), .CLKOUT6()
    );
    BUFG u_bufg_fb  (.I(clk_fb),       .O(clk_fb_buf));
    BUFG u_bufg_pix (.I(pix_clk_mmcm), .O(pix_clk));
    BUFG u_bufg_ser (.I(ser_clk_mmcm), .O(ser_clk));

    // 像素时钟域复位：MMCM 锁定后保持若干拍再释放
    reg [7:0] rst_cnt = 8'd0;
    reg       pix_rst = 1'b1;
    always @(posedge pix_clk or negedge mmcm_locked) begin
        if (!mmcm_locked) begin
            rst_cnt <= 8'd0;
            pix_rst <= 1'b1;
        end else if (rst_cnt != 8'hFF) begin
            rst_cnt <= rst_cnt + 1'b1;
            pix_rst <= 1'b1;
        end else begin
            pix_rst <= 1'b0;
        end
    end

    // -------------------------------------------------------------------------
    // 640x480@60 时序：行 800（640 + 16 + 96 + 48），场 525（480 + 10 + 2 + 33），同步负极性
    // -------------------------------------------------------------------------
    reg [9:0] hc, vc;
    always @(posedge pix_clk) begin
        if (pix_rst) begin
            hc <= 10'd0;
            vc <= 10'd0;
            frame_cnt_pix <= 24'd0;
        end else if (hc == 10'd799) begin
            hc <= 10'd0;
            if (vc == 10'd524) begin
                vc <= 10'd0;
                frame_cnt_pix <= frame_cnt_pix + 1'b1;
            end else begin
                vc <= vc + 1'b1;
            end
        end else begin
            hc <= hc + 1'b1;
        end
    end
    always @(posedge pix_clk)
        hpd_sync <= {hpd_sync[1:0], hdmi_hpd};

    wire       de0 = (hc < 10'd640) && (vc < 10'd480);
    wire       hs0 = !((hc >= 10'd656) && (hc < 10'd752));
    wire       vs0 = !((vc >= 10'd490) && (vc < 10'd492));
    // 行首字地址 vc*80 = vc*64 + vc*16
    wire [15:0] line_base = {vc, 6'd0} + {2'd0, vc, 4'd0};

    // 第 0 拍出地址，第 2 拍得到数据：同步信号与像素序号延迟 2 拍对齐
    reg [2:0] de_d, hs_d, vs_d;
    reg [2:0] px_d0, px_d1;
    always @(posedge pix_clk) begin
        b_addr <= line_base + {6'd0, hc[9:3]};
        de_d   <= {de_d[1:0], de0};
        hs_d   <= {hs_d[1:0], hs0};
        vs_d   <= {vs_d[1:0], vs0};
        px_d0  <= hc[2:0];
        px_d1  <= px_d0;
    end
    reg [2:0] px_d2;
    always @(posedge pix_clk)
        px_d2 <= px_d1;

    // 取出当前像素的 4bit 颜色索引
    wire [3:0] idx = b_rdata_q[{px_d2, 2'b00} +: 4];

    // 16 色调色板（与 sw/src/hdmi_ui.h 中颜色编号一致）
    reg [23:0] rgb;
    always @(*) begin
        case (idx)
        4'd0:  rgb = 24'h0A0E1A;    // 背景：深蓝黑
        4'd1:  rgb = 24'hFFFFFF;    // 白
        4'd2:  rgb = 24'h3A4256;    // 深灰
        4'd3:  rgb = 24'h8A93A8;    // 灰
        4'd4:  rgb = 24'hFF3030;    // 红
        4'd5:  rgb = 24'h30E060;    // 绿
        4'd6:  rgb = 24'h3080FF;    // 蓝
        4'd7:  rgb = 24'hFFD030;    // 黄
        4'd8:  rgb = 24'h30E0E0;    // 青
        4'd9:  rgb = 24'hE040E0;    // 品红
        4'd10: rgb = 24'hFF8C20;    // 橙
        4'd11: rgb = 24'h142040;    // 深蓝（面板）
        4'd12: rgb = 24'h104020;    // 深绿
        4'd13: rgb = 24'h501010;    // 深红
        4'd14: rgb = 24'hC8CED8;    // 浅灰
        default: rgb = 24'h000000;  // 黑
        endcase
    end

    reg  [23:0] rgb_q;
    reg         de_q, hs_q, vs_q;
    always @(posedge pix_clk) begin
        rgb_q <= de_d[2] ? rgb : 24'd0;
        de_q  <= de_d[2];
        hs_q  <= hs_d[2];
        vs_q  <= vs_d[2];
    end

    // -------------------------------------------------------------------------
    // TMDS 编码与串行输出：通道 0 = 蓝（携带 HS/VS），1 = 绿，2 = 红
    // -------------------------------------------------------------------------
    wire [9:0] sym0, sym1, sym2;
    tmds_encoder u_enc0 (.clk(pix_clk), .rst(pix_rst), .d(rgb_q[7:0]),   .c0(hs_q), .c1(vs_q), .de(de_q), .q(sym0));
    tmds_encoder u_enc1 (.clk(pix_clk), .rst(pix_rst), .d(rgb_q[15:8]),  .c0(1'b0), .c1(1'b0), .de(de_q), .q(sym1));
    tmds_encoder u_enc2 (.clk(pix_clk), .rst(pix_rst), .d(rgb_q[23:16]), .c0(1'b0), .c1(1'b0), .de(de_q), .q(sym2));

    tmds_serializer u_ser0 (.pix_clk(pix_clk), .ser_clk(ser_clk), .rst(pix_rst), .d(sym0),          .p(tmds_data_p[0]), .n(tmds_data_n[0]));
    tmds_serializer u_ser1 (.pix_clk(pix_clk), .ser_clk(ser_clk), .rst(pix_rst), .d(sym1),          .p(tmds_data_p[1]), .n(tmds_data_n[1]));
    tmds_serializer u_ser2 (.pix_clk(pix_clk), .ser_clk(ser_clk), .rst(pix_rst), .d(sym2),          .p(tmds_data_p[2]), .n(tmds_data_n[2]));
    tmds_serializer u_serc (.pix_clk(pix_clk), .ser_clk(ser_clk), .rst(pix_rst), .d(10'b0000011111), .p(tmds_ck_p),     .n(tmds_ck_n));

endmodule


// =============================================================================
// DVI 1.0 TMDS 编码（8b/10b，直流平衡），一拍延迟
// =============================================================================
module tmds_encoder (
    input  wire       clk,
    input  wire       rst,
    input  wire [7:0] d,
    input  wire       c0,
    input  wire       c1,
    input  wire       de,
    output reg  [9:0] q
);
    // 第一步：XOR / XNOR 最小化跳变
    wire [3:0] n1d = d[0] + d[1] + d[2] + d[3] + d[4] + d[5] + d[6] + d[7];
    wire       use_xnor = (n1d > 4'd4) || (n1d == 4'd4 && d[0] == 1'b0);
    wire [8:0] qm;
    assign qm[0] = d[0];
    genvar i;
    generate
        for (i = 1; i < 8; i = i + 1) begin : g_qm
            assign qm[i] = use_xnor ? ~(qm[i-1] ^ d[i]) : (qm[i-1] ^ d[i]);
        end
    endgenerate
    assign qm[8] = ~use_xnor;

    // 第二步：根据累计直流偏差决定是否取反
    wire [3:0] n1q = qm[0] + qm[1] + qm[2] + qm[3] + qm[4] + qm[5] + qm[6] + qm[7];
    wire signed [4:0] diff = $signed({1'b0, n1q}) - 5'sd4;      // (N1 - N0) / 2
    reg  signed [4:0] cnt;                                       // 累计偏差（以 1 计的一半）

    always @(posedge clk) begin
        if (rst) begin
            cnt <= 5'sd0;
            q   <= 10'b1101010100;
        end else if (!de) begin
            cnt <= 5'sd0;
            case ({c1, c0})
            2'b00: q <= 10'b1101010100;
            2'b01: q <= 10'b0010101011;
            2'b10: q <= 10'b0101010100;
            default: q <= 10'b1010101011;
            endcase
        end else if (cnt == 0 || diff == 0) begin
            q   <= {~qm[8], qm[8], qm[8] ? qm[7:0] : ~qm[7:0]};
            cnt <= qm[8] ? cnt + diff : cnt - diff;
        end else if ((cnt > 0 && diff > 0) || (cnt < 0 && diff < 0)) begin
            q   <= {1'b1, qm[8], ~qm[7:0]};
            cnt <= cnt + {{4{1'b0}}, qm[8]} - diff;
        end else begin
            q   <= {1'b0, qm[8], qm[7:0]};
            cnt <= cnt - {{4{1'b0}}, ~qm[8]} + diff;
        end
    end
endmodule


// =============================================================================
// 10:1 串行化（OSERDESE2 主从级联，DDR，低位先发）+ TMDS_33 差分输出
// =============================================================================
module tmds_serializer (
    input  wire       pix_clk,
    input  wire       ser_clk,
    input  wire       rst,
    input  wire [9:0] d,
    output wire       p,
    output wire       n
);
    wire oq, shift1, shift2;

    OSERDESE2 #(
        .DATA_RATE_OQ("DDR"), .DATA_RATE_TQ("SDR"), .DATA_WIDTH(10),
        .SERDES_MODE("MASTER"), .TRISTATE_WIDTH(1)
    ) u_master (
        .OQ(oq), .OFB(), .TQ(), .TFB(), .SHIFTOUT1(), .SHIFTOUT2(), .TBYTEOUT(),
        .CLK(ser_clk), .CLKDIV(pix_clk),
        .D1(d[0]), .D2(d[1]), .D3(d[2]), .D4(d[3]), .D5(d[4]), .D6(d[5]), .D7(d[6]), .D8(d[7]),
        .TCE(1'b0), .OCE(1'b1), .TBYTEIN(1'b0), .RST(rst),
        .SHIFTIN1(shift1), .SHIFTIN2(shift2),
        .T1(1'b0), .T2(1'b0), .T3(1'b0), .T4(1'b0)
    );
    OSERDESE2 #(
        .DATA_RATE_OQ("DDR"), .DATA_RATE_TQ("SDR"), .DATA_WIDTH(10),
        .SERDES_MODE("SLAVE"), .TRISTATE_WIDTH(1)
    ) u_slave (
        .OQ(), .OFB(), .TQ(), .TFB(), .SHIFTOUT1(shift1), .SHIFTOUT2(shift2), .TBYTEOUT(),
        .CLK(ser_clk), .CLKDIV(pix_clk),
        .D1(1'b0), .D2(1'b0), .D3(d[8]), .D4(d[9]), .D5(1'b0), .D6(1'b0), .D7(1'b0), .D8(1'b0),
        .TCE(1'b0), .OCE(1'b1), .TBYTEIN(1'b0), .RST(rst),
        .SHIFTIN1(1'b0), .SHIFTIN2(1'b0),
        .T1(1'b0), .T2(1'b0), .T3(1'b0), .T4(1'b0)
    );
    OBUFDS #(.IOSTANDARD("TMDS_33")) u_obuf (.I(oq), .O(p), .OB(n));
endmodule
