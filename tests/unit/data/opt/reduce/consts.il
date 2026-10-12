module \top
  wire input 1 \a
  wire output 2 \y1
  wire output 3 \y2
  wire output 4 \y3
  wire output 5 \y4
  cell $reduce_or \one
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A { 1'1 \a }
    connect \Y \y1
  end
  cell $reduce_and \zero
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A { 1'0 \a }
    connect \Y \y2
  end
  cell $reduce_and \empty_and
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A 2'11
    connect \Y \y3
  end
  cell $reduce_or \empty_or
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A 2'00
    connect \Y \y4
  end
end
