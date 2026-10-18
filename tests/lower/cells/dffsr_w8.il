attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \set
  wire input 3 \clr
  wire width 8 input 4 \d
  wire width 8 output 5 \q
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  wire $n34
  wire $n35
  wire $n36
  wire $n37
  wire $n46
  wire $n47
  wire $n48
  wire $n49
  wire $n50
  wire $n51
  wire $n52
  wire $n53
  wire $n54
  wire $n55
  wire $n56
  wire $n57
  wire $n58
  wire $n59
  wire $n60
  wire $n61
  wire $n70
  wire $n71
  wire $n72
  wire $n73
  wire $n74
  wire $n75
  wire $n76
  wire $n77
  cell $mux $c9
    parameter \WIDTH 8
    connect \A 8'00000000
    connect \B 8'10100101
    connect \S \set
    connect \Y { $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 }
  end
  cell $mux $c11
    parameter \WIDTH 8
    connect \A 8'00000000
    connect \B 8'01011010
    connect \S \set
    connect \Y { $n53 $n52 $n51 $n50 $n49 $n48 $n47 $n46 }
  end
  cell $mux $c12
    parameter \WIDTH 8
    connect \A { $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 }
    connect \B 8'00000000
    connect \S \clr
    connect \Y { $n61 $n60 $n59 $n58 $n57 $n56 $n55 $n54 }
  end
  cell $mux $c14
    parameter \WIDTH 8
    connect \A { $n53 $n52 $n51 $n50 $n49 $n48 $n47 $n46 }
    connect \B 8'11111111
    connect \S \clr
    connect \Y { $n77 $n76 $n75 $n74 $n73 $n72 $n71 $n70 }
  end
  cell $dffsr $c15
    parameter \WIDTH 8
    parameter \CLK_POLARITY 1
    parameter \SET_POLARITY 1
    parameter \CLR_POLARITY 1
    connect \CLK \clk
    connect \SET { $n61 $n60 $n59 $n58 $n57 $n56 $n55 $n54 }
    connect \CLR { $n77 $n76 $n75 $n74 $n73 $n72 $n71 $n70 }
    connect \D \d
    connect \Q \q
  end
end
