# generated: elab-check --write
attribute \top 1
module \e2_reduction
  wire width 4 input 1 \a
  wire width 8 input 2 \w
  wire output 3 \y1
  wire output 4 \y2
  wire output 5 \y3
  wire output 6 \y4
  wire output 7 \y5
  wire output 8 \y6
  wire output 9 \y7
  wire output 10 \y8
  wire output 11 \y9
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  cell $reduce_and $c12
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y \y1
  end
  cell $reduce_and $c13
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y $n22
  end
  cell $logic_not $c14
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n22
    connect \Y \y2
  end
  cell $reduce_or $c15
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y \y3
  end
  cell $reduce_or $c16
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y $n23
  end
  cell $logic_not $c17
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n23
    connect \Y \y4
  end
  cell $reduce_xor $c18
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y \y5
  end
  cell $reduce_xnor $c19
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y \y6
  end
  cell $reduce_xnor $c20
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y \y7
  end
  cell $reduce_xor $c21
    parameter \A_SIGNED 0
    parameter \A_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A \w
    connect \Y \y8
  end
  cell $reduce_and $c22
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y $n24
  end
  cell $reduce_xor $c23
    parameter \A_SIGNED 0
    parameter \A_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A \w
    connect \Y $n25
  end
  cell $or $c24
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n24
    connect \B $n25
    connect \Y \y9
  end
end
