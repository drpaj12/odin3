-- Uses pkg_b, which uses pkg_a: neither can be analysed first.
use work.pkg_b.all;

package pkg_a is
    constant A : natural := B + 1;
end package pkg_a;
