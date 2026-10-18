attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \en
  wire input 3 \set
  wire input 4 \clr
  wire width 8 input 5 \d
  wire width 8 output 6 \q
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n44
  wire $n45
  wire $n46
  wire $n55
  wire $n56
  wire $n57
  wire $n58
  wire $n59
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
  wire $n79
  wire $n80
  wire $n81
  wire $n82
  wire $n83
  wire $n84
  wire $n85
  wire $n86
  cell $mux $c11
    parameter \WIDTH 8
    connect \A 8'00000000
    connect \B 8'10100101
    connect \S \set
    connect \Y { $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 }
  end
  cell $mux $c13
    parameter \WIDTH 8
    connect \A 8'00000000
    connect \B 8'01011010
    connect \S \set
    connect \Y { $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 }
  end
  cell $mux $c14
    parameter \WIDTH 8
    connect \A { $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 }
    connect \B 8'00000000
    connect \S \clr
    connect \Y { $n70 $n69 $n68 $n67 $n66 $n65 $n64 $n63 }
  end
  cell $mux $c16
    parameter \WIDTH 8
    connect \A { $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 }
    connect \B 8'11111111
    connect \S \clr
    connect \Y { $n86 $n85 $n84 $n83 $n82 $n81 $n80 $n79 }
  end
  cell $dffsre $c18
    parameter \WIDTH 8
    parameter \CLK_POLARITY 1
    parameter \SET_POLARITY 1
    parameter \CLR_POLARITY 1
    parameter \EN_POLARITY 1
    connect \CLK \clk
    connect \SET { $n70 $n69 $n68 $n67 $n66 $n65 $n64 $n63 }
    connect \CLR { $n86 $n85 $n84 $n83 $n82 $n81 $n80 $n79 }
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
