module \top
  wire input 1 \clk
  wire input 2 \d
  wire input 3 \a
  wire output 4 \r
  wire output 5 \q
  wire output 6 \y
  wire output 7 \z
  wire \u_r.regs
  wire \u.w
  wire \u_r.clk
  wire \u_r.d
  wire \u_r.q
  wire width 32 \u_r.regs_1
  wire \u.a
  wire \u.y
  wire \u.w_1
  cell $dff $c12
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D 1'x
    connect \Q \r
  end
  cell $dff $c13
    parameter \WIDTH 32
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D { \u_r.regs_1 [30:1] \q \d }
    connect \Q { \u_r.regs_1 [31:1] \q }
  end
  cell $not $c14
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y \y
  end
  connect \z \u.w
  connect \u_r.clk \clk
  connect \u_r.d \d
  connect \u_r.q \q
  connect \u_r.regs_1 [0] \q
  connect \u.a \a
  connect \u.y \y
  connect \u.w_1 \y
end
