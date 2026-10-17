attribute \top 1
module \m
  wire width 2 input 1 \a
  wire output 2 \y
  cell $reduce_xor $c3
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y \y
  end
end
