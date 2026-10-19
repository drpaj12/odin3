attribute \top 1
module \cascade
  wire input 1 \clk
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 input 4 \c
  wire width 8 output 5 \y
  wire width 8 \r
  wire width 8 \t
  wire $n47
  wire $n48
  wire $n49
  wire $n50
  wire $n51
  wire $n60
  wire $n61
  wire $n62
  wire $n63
  wire $n64
  wire $n65
  wire $n66
  wire $n67
  wire $n68
  wire $n69
  wire $n70
  cell $add $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 5
    connect \A \a
    connect \B \b
    connect \Y { $n51 $n50 $n49 $n48 $n47 }
  end
  cell $dff $c13
    parameter \WIDTH 5
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D { $n51 $n50 $n49 $n48 $n47 }
    connect \Q { $n64 $n63 $n62 $n61 $n60 }
  end
  cell $add $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 5
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 6
    connect \A { $n64 $n63 $n62 $n61 $n60 }
    connect \B \c
    connect \Y { $n70 $n69 $n68 $n67 $n66 $n65 }
  end
  connect \y { 2'00 $n70 $n69 $n68 $n67 $n66 $n65 }
  connect \r { 3'000 $n64 $n63 $n62 $n61 $n60 }
  connect \t { 3'000 $n51 $n50 $n49 $n48 $n47 }
end
