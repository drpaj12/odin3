attribute \top 1
module \t
  wire input 1 \s
  wire input 2 \u
  wire input 3 \v
  wire width 4 input 4 \a
  wire width 4 input 5 \b
  wire width 4 input 6 \c
  wire width 4 output 7 \y
  wire width 4 output 8 \z
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n44
  wire $n45
  wire $n46
  wire $n47
  wire $n48
  wire $n49
  wire $n50
  wire $n51
  wire $n61
  wire $n62
  wire $n63
  wire $n64
  wire $n70
  wire $n71
  wire $n72
  wire $n73
  wire $n74
  wire $n75
  wire $n76
  wire $n77
  cell $xor $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n51 $n50 $n49 $n48 }
  end
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 4
    connect \A \c
    connect \B \a
    connect \S \s
    connect \Y \z
  end
  attribute \full_case 1
  cell $mux $c13
    parameter \WIDTH 4
    connect \A { $n51 $n50 $n49 $n48 }
    connect \B { $n43 $n42 $n41 $n40 }
    connect \S \s
    connect \Y \y
  end
  attribute \full_case 1
  cell $mux $c14
    parameter \WIDTH 4
    connect \A \c
    connect \B { $n47 $n46 $n45 $n44 }
    connect \S \u
    connect \Y { $n64 $n63 $n62 $n61 }
  end
  attribute \full_case 1
  cell $mux $c16
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B { $n64 $n63 $n62 $n61 }
    connect \S \s
    connect \Y { $n43 $n42 $n41 $n40 }
  end
  attribute \full_case 1
  cell $mux $c17
    parameter \WIDTH 4
    connect \A \b
    connect \B \a
    connect \S \v
    connect \Y { $n73 $n72 $n71 $n70 }
  end
  attribute \full_case 1
  cell $mux $c18
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B { $n73 $n72 $n71 $n70 }
    connect \S \u
    connect \Y { $n77 $n76 $n75 $n74 }
  end
  attribute \full_case 1
  cell $mux $c19
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B { $n77 $n76 $n75 $n74 }
    connect \S \s
    connect \Y { $n47 $n46 $n45 $n44 }
  end
end
